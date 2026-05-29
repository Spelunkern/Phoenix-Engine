#include "audio/audio_system.h"
#include "character/character_system.h"
#include "character/weapon_effect.h"
#include "core/logging.h"
#include "runtime/phoenix_runtime.h"
#include "platform/sdl_window.h"
#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"
#include "ui/editor_panel.h"
#include "ui/perf_hud.h"
#include "world/actor_scene.h"
#include "generated/loading_icon_bgra.inc"

#include "imgui.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <psapi.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")
#endif

namespace
{
    // UI types/functions live in ui/editor_panel.{h,cpp} and ui/perf_hud.{h,cpp}.
    using phoenix::ui::WeatherMode;
    using phoenix::ui::CharacterOption;
    using phoenix::ui::UnifiedPanelResult;
    using phoenix::ui::apply_renderer_fog;
    using phoenix::ui::draw_editor_panel;
    using phoenix::ui::PerfHudState;
    using phoenix::ui::draw_perf_hud;

    constexpr const char* kAppTitle = "Phoenix Engine";
    constexpr std::size_t kWaterTextureLayer = 62;
    constexpr std::size_t kSkyTextureLayer = 63;
    constexpr std::size_t kPrimaryCloudTextureLayer = 64;
    constexpr std::size_t kSecondaryCloudTextureLayer = 65;
    constexpr std::size_t kAssetTextureLayerBase = 66;
    constexpr std::size_t kMaxLoggedAssetTextures = 80;
    // Atmospheric rendering: fogEnd must be well inside viewDistance so
    // exponential fog fully covers geometry before the cull boundary.
    // This eliminates pop-in without needing alpha blending on opaques.
    constexpr float kTanHalfFov = 0.7002f;
    constexpr float kSoundAudibleRadiusScale = 1.6f;
    constexpr float kSoundAudibleRadiusBonus = 16.0f;
    constexpr float kNamePlateVisibleDistance = 20.0f;
    constexpr float kWeatherWaterY = 0.0f;


    // Height sampler context � terrain + collision mesh floor surfaces.
    struct HeightSamplerContext
    {
        const phoenix::runtime::PhoenixRuntime* runtime{};
        const phoenix::runtime::WorldCollisionMesh* collisionMesh{};
        mutable float lastCharacterY{}; // updated each frame from character system
    };

    constexpr float kStepHeight = 1.5f; // max height character can step up onto per frame

    // Height sampler callback for character terrain following.
    float character_height_sampler(float worldX, float worldZ, void* userData)
    {
        const auto* ctx = static_cast<const HeightSamplerContext*>(userData);
        float terrainY = ctx->runtime->terrain_height_at(worldX, worldZ);

        // Also check walkable collision surfaces (bridge decks, ramps, stairs).
        // We use the character's characterY stored externally for step-height limiting.
        if (ctx->collisionMesh && !ctx->collisionMesh->triangles.empty())
        {
            // Query with characterY context: allow stepping up from current position.
            float refY = std::max(terrainY, ctx->lastCharacterY);
            float floorY = ctx->collisionMesh->floor_height_at(
                worldX, worldZ, refY, kStepHeight);
            if (floorY > terrainY)
                terrainY = floorY;
        }

        return terrainY;
    }

    // Collision callback � triangle mesh collision against world objects.
    constexpr float kCharacterRadius = 0.6f;
    constexpr float kCharacterHeight = 2.2f;

    bool character_collision_callback(float proposedX, float proposedZ,
        float previousX, float previousZ,
        float characterY,
        float& outX, float& outZ, void* userData)
    {
        const auto* collisionMesh = static_cast<const phoenix::runtime::WorldCollisionMesh*>(userData);
        outX = proposedX;
        outZ = proposedZ;
        return collisionMesh->check_collision(previousX, previousZ, outX, outZ,
            characterY, kCharacterHeight, kCharacterRadius);
    }

    struct PlayableSpawn
    {
        float x{};
        float y{};
        float z{};
        bool valid{};
    };

    PlayableSpawn find_dungeon_playable_spawn(
        const phoenix::runtime::WorldCollisionMesh& collisionMesh,
        float preferredX,
        float preferredY,
        float preferredZ)
    {
        PlayableSpawn best{};
        float bestScore = std::numeric_limits<float>::max();
        for (const auto& tri : collisionMesh.triangles)
        {
            if (tri.normalY < phoenix::runtime::WorldCollisionMesh::kWalkableNormalY)
                continue;

            const float cx = (tri.v0[0] + tri.v1[0] + tri.v2[0]) / 3.0f;
            const float cy = (tri.v0[1] + tri.v1[1] + tri.v2[1]) / 3.0f;
            const float cz = (tri.v0[2] + tri.v1[2] + tri.v2[2]) / 3.0f;

            const float ax = tri.v1[0] - tri.v0[0];
            const float ay = tri.v1[1] - tri.v0[1];
            const float az = tri.v1[2] - tri.v0[2];
            const float bx = tri.v2[0] - tri.v0[0];
            const float by = tri.v2[1] - tri.v0[1];
            const float bz = tri.v2[2] - tri.v0[2];
            const float nx = ay * bz - az * by;
            const float ny = az * bx - ax * bz;
            const float nz = ax * by - ay * bx;
            const float area = std::sqrt(nx * nx + ny * ny + nz * nz) * 0.5f;
            if (area < 0.15f)
                continue;

            const float dx = cx - preferredX;
            const float dy = cy - preferredY;
            const float dz = cz - preferredZ;
            const float score = dx * dx + dz * dz + dy * dy * 0.04f - std::min(area, 64.0f) * 0.25f;
            if (score < bestScore)
            {
                bestScore = score;
                best = { cx, cy + 0.04f, cz, true };
            }
        }
        return best;
    }


    std::filesystem::path executable_directory()
    {
#ifdef _WIN32
        std::wstring path(MAX_PATH, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return std::filesystem::current_path();
        path.resize(length);
        return std::filesystem::path(path).parent_path();
#else
        return std::filesystem::canonical("/proc/self/exe").parent_path();
#endif
    }



    struct CameraView
    {
        float x{};
        float y{};
        float z{};
        float yaw{};
        float pitch{};
        float aspect{ 1.0f };
        float distance{ 5000.0f };
    };

    struct MapAudioScene
    {
        struct MusicZone
        {
            std::filesystem::path path;
            phoenix::world::WldBoundingBox box{};
            float fadeDistance{};
        };

        struct SoundEmitter
        {
            std::filesystem::path path;
            float x{};
            float y{};
            float z{};
            float radius{};
        };

        std::vector<MusicZone> musicZones;
        std::vector<SoundEmitter> soundEmitters;
    };



    std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::vector<int> scan_csv_indices(const std::filesystem::path& csvPath)
    {
        std::vector<int> indices;
        std::ifstream file(csvPath);
        if (!file)
            return indices;
        std::string line;
        std::getline(file, line); // skip header
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::string clean;
            clean.reserve(line.size());
            for (char c : line)
                if (c != '"') clean += c;
            auto comma = clean.find(',');
            if (comma == std::string::npos) continue;
            indices.push_back(std::atoi(clean.substr(0, comma).c_str()));
        }
        return indices;
    }

    std::vector<CharacterOption> scan_character_options(const std::filesystem::path& dataRoot)
    {
        const auto characterRoot = dataRoot / "Character";
        if (!std::filesystem::exists(characterRoot))
            return {};

        constexpr std::string_view partFiles[] = { "upper", "lower", "hand", "foot", "helmet", "face", "hair" };

        struct Temp
        {
            std::string raceFolder;
            std::string prefix;
            std::vector<int> upper, lower, hand, foot, helmet, face, hair;
        };

        std::map<std::string, Temp> byKey;
        for (const auto& raceEntry : std::filesystem::directory_iterator(characterRoot))
        {
            if (!raceEntry.is_directory())
                continue;
            const auto raceFolder = raceEntry.path().filename().string();
            for (const auto& entry : std::filesystem::directory_iterator(raceEntry.path()))
            {
                if (!entry.is_regular_file())
                    continue;
                auto ext = lower_ascii(entry.path().extension().string());
                if (ext != ".csv")
                    continue;
                auto stem = lower_ascii(entry.path().stem().string());
                if (stem.find("_action") != std::string::npos)
                    continue;

                for (const auto partName : partFiles)
                {
                    auto suffix = "_" + std::string(partName);
                    if (!stem.ends_with(suffix))
                        continue;
                    auto prefix = stem.substr(0, stem.size() - suffix.size());
                    auto key = raceFolder + "|" + prefix;
                    auto& temp = byKey[key];
                    temp.raceFolder = raceFolder;
                    temp.prefix = prefix;
                    auto indices = scan_csv_indices(entry.path());
                    if (partName == "upper") temp.upper = std::move(indices);
                    else if (partName == "lower") temp.lower = std::move(indices);
                    else if (partName == "hand") temp.hand = std::move(indices);
                    else if (partName == "foot") temp.foot = std::move(indices);
                    else if (partName == "helmet") temp.helmet = std::move(indices);
                    else if (partName == "face") temp.face = std::move(indices);
                    else if (partName == "hair") temp.hair = std::move(indices);
                    break;
                }
            }
        }

        std::vector<CharacterOption> options;
        options.reserve(byKey.size());
        for (auto& [_, temp] : byKey)
        {
            if (temp.face.empty() || temp.hair.empty())
                continue;
            CharacterOption option{};
            option.raceFolder = temp.raceFolder;
            option.prefix = temp.prefix;
            option.label = temp.raceFolder + " / " + temp.prefix;
            option.upperIndices = std::move(temp.upper);
            option.lowerIndices = std::move(temp.lower);
            option.handIndices = std::move(temp.hand);
            option.footIndices = std::move(temp.foot);
            option.helmetIndices = std::move(temp.helmet);
            option.faceIndices = std::move(temp.face);
            option.hairIndices = std::move(temp.hair);
            options.push_back(std::move(option));
        }

        std::ranges::sort(options, [](const auto& lhs, const auto& rhs) {
            return lhs.label < rhs.label;
        });
        return options;
    }


    float distance_to_box(float x, float y, float z, const phoenix::world::WldBoundingBox& box)
    {
        const float minX = std::min(box.min[0], box.max[0]);
        const float minY = std::min(box.min[1], box.max[1]);
        const float minZ = std::min(box.min[2], box.max[2]);
        const float maxX = std::max(box.min[0], box.max[0]);
        const float maxY = std::max(box.min[1], box.max[1]);
        const float maxZ = std::max(box.min[2], box.max[2]);
        const float dx = x < minX ? minX - x : (x > maxX ? x - maxX : 0.0f);
        const float dy = y < minY ? minY - y : (y > maxY ? y - maxY : 0.0f);
        const float dz = z < minZ ? minZ - z : (z > maxZ ? z - maxZ : 0.0f);
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    float smooth_falloff(float normalizedDistance)
    {
        const auto t = std::clamp(normalizedDistance, 0.0f, 1.0f);
        return 1.0f - (t * t * (3.0f - 2.0f * t));
    }

    MapAudioScene build_map_audio_scene(const phoenix::runtime::PhoenixRuntime& runtime)
    {
        MapAudioScene scene;
        const auto& world = runtime.state().world;
        if (!world.parsed)
            return scene;

        const auto mapSize = static_cast<float>(std::max(1u, world.mapSize));
        const auto halfMap = world.isDungeon ? 0.0f : mapSize * 0.5f;
        const auto worldX = [&](float rawX) { return rawX - halfMap; };
        const auto worldZ = [&](float rawZ) { return rawZ - halfMap; };

        scene.musicZones.reserve(world.musicZones.size());
        for (const auto& zone : world.musicZones)
        {
            if (zone.musicAssetId < 0 || static_cast<std::size_t>(zone.musicAssetId) >= world.musicAssets.size())
                continue;

            auto path = runtime.audio_path_for(world.musicAssets[static_cast<std::size_t>(zone.musicAssetId)]);
            if (path.empty())
                continue;

            MapAudioScene::MusicZone mapped{};
            mapped.path = std::move(path);
            mapped.box = zone.box;
            mapped.box.min[0] = worldX(mapped.box.min[0]);
            mapped.box.max[0] = worldX(mapped.box.max[0]);
            mapped.box.min[2] = worldZ(mapped.box.min[2]);
            mapped.box.max[2] = worldZ(mapped.box.max[2]);
            mapped.fadeDistance = std::max(48.0f, zone.radius);
            scene.musicZones.push_back(std::move(mapped));
        }

        scene.soundEmitters.reserve(world.soundEffects.size());
        for (const auto& sound : world.soundEffects)
        {
            if (sound.soundEffectAssetId < 0
                || static_cast<std::size_t>(sound.soundEffectAssetId) >= world.soundEffectAssets.size()
                || sound.radius <= 0.0f)
            {
                continue;
            }

            auto path = runtime.audio_path_for(world.soundEffectAssets[static_cast<std::size_t>(sound.soundEffectAssetId)]);
            if (path.empty())
                continue;

            MapAudioScene::SoundEmitter emitter{};
            emitter.path = std::move(path);
            emitter.x = worldX(sound.center[0]);
            emitter.y = sound.center[1];
            emitter.z = worldZ(sound.center[2]);
            emitter.radius = std::max(48.0f, sound.radius * kSoundAudibleRadiusScale + kSoundAudibleRadiusBonus);
            scene.soundEmitters.push_back(std::move(emitter));
        }

        return scene;
    }

    std::vector<phoenix::audio::AudibleTrack> build_audible_tracks(
        const MapAudioScene& scene,
        float listenerX,
        float listenerY,
        float listenerZ,
        bool enableMusic,
        bool enableSounds)
    {
        std::vector<phoenix::audio::AudibleTrack> tracks;
        tracks.reserve(16);

        if (enableMusic)
        {
            for (const auto& zone : scene.musicZones)
            {
                const auto distance = distance_to_box(listenerX, listenerY, listenerZ, zone.box);
                if (distance > zone.fadeDistance)
                    continue;

                phoenix::audio::AudibleTrack track{};
                track.path = zone.path;
                track.volume = 0.72f * smooth_falloff(distance / zone.fadeDistance);
                track.music = true;
                tracks.push_back(std::move(track));
            }
        }

        if (enableSounds)
        {
            struct Candidate
            {
                phoenix::audio::AudibleTrack track;
                float volume{};
            };

            std::vector<Candidate> candidates;
            candidates.reserve(std::min<std::size_t>(scene.soundEmitters.size(), 64));
            for (const auto& emitter : scene.soundEmitters)
            {
                const auto dx = listenerX - emitter.x;
                const auto dy = listenerY - emitter.y;
                const auto dz = listenerZ - emitter.z;
                const auto distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (distance > emitter.radius)
                    continue;

                const auto volume = 0.86f * smooth_falloff(distance / emitter.radius);
                phoenix::audio::AudibleTrack track{};
                track.path = emitter.path;
                track.volume = volume;
                track.music = false;
                candidates.push_back({ std::move(track), volume });
            }

            std::ranges::sort(candidates, [](const auto& lhs, const auto& rhs) {
                return lhs.volume > rhs.volume;
            });
            constexpr std::size_t kMaxAmbientVoices = 24;
            for (std::size_t i = 0; i < std::min(kMaxAmbientVoices, candidates.size()); ++i)
                tracks.push_back(std::move(candidates[i].track));
        }

        return tracks;
    }

    bool sphere_visible(const CameraView& view, float worldX, float worldY, float worldZ, float radius)
    {
        const float dx = worldX - view.x;
        const float dy = worldY - view.y;
        const float dz = worldZ - view.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        const float maxDistance = view.distance + radius;
        if (distanceSq > maxDistance * maxDistance)
            return false;

        const float cy = std::cos(view.yaw);
        const float sy = std::sin(view.yaw);
        const float cp = std::cos(view.pitch);
        const float sp = std::sin(view.pitch);

        const float cameraX = cy * dx - sy * dz;
        const float yawZ = sy * dx + cy * dz;
        const float cameraY = cp * dy - sp * yawZ;
        const float cameraZ = sp * dy + cp * yawZ;
        if (cameraZ < -radius)
            return false;

        const float zForBounds = std::max(cameraZ, 1.0f);
        const float horizontal = zForBounds * kTanHalfFov * view.aspect + radius;
        const float vertical = zForBounds * kTanHalfFov + radius;
        return std::abs(cameraX) <= horizontal && std::abs(cameraY) <= vertical;
    }

    bool project_world_to_screen(
        const CameraView& view,
        float worldX,
        float worldY,
        float worldZ,
        float width,
        float height,
        ImVec2& out)
    {
        const float dx = worldX - view.x;
        const float dy = worldY - view.y;
        const float dz = worldZ - view.z;
        const float cy = std::cos(view.yaw);
        const float sy = std::sin(view.yaw);
        const float cp = std::cos(view.pitch);
        const float sp = std::sin(view.pitch);

        const float cameraX = cy * dx - sy * dz;
        const float yawZ = sy * dx + cy * dz;
        const float cameraY = cp * dy - sp * yawZ;
        const float cameraZ = sp * dy + cp * yawZ;
        if (cameraZ <= 1.0f)
            return false;

        const float ndcX = cameraX / (cameraZ * kTanHalfFov * view.aspect);
        const float ndcY = cameraY / (cameraZ * kTanHalfFov);
        if (std::abs(ndcX) > 1.2f || std::abs(ndcY) > 1.2f)
            return false;

        out.x = (ndcX * 0.5f + 0.5f) * width;
        out.y = (0.5f - ndcY * 0.5f) * height;
        return true;
    }

    float hash01(std::uint32_t value)
    {
        value ^= value >> 16;
        value *= 0x7feb352du;
        value ^= value >> 15;
        value *= 0x846ca68bu;
        value ^= value >> 16;
        return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x01000000u);
    }

    float wrap01(float value)
    {
        return value - std::floor(value);
    }

    void draw_weather_overlay(
        WeatherMode weatherMode,
        const CameraView& view,
        float totalTime,
        float width,
        float height)
    {
        if (weatherMode == WeatherMode::Default || width <= 0.0f || height <= 0.0f)
            return;

        auto* background = ImGui::GetBackgroundDrawList();
        auto* drawList = ImGui::GetForegroundDrawList();
        if (weatherMode == WeatherMode::Storm)
        {
            background->AddRectFilled(
                ImVec2(0.0f, 0.0f),
                ImVec2(width, height),
                IM_COL32(24, 28, 34, 20));

            const float lightningCycle = std::fmod(totalTime + 2.35f, 8.7f);
            float lightning = 0.0f;
            if (lightningCycle < 0.10f)
                lightning = 1.0f - lightningCycle / 0.10f;
            else if (lightningCycle > 0.19f && lightningCycle < 0.28f)
                lightning = 0.62f * (1.0f - (lightningCycle - 0.19f) / 0.09f);
            if (lightning > 0.0f)
            {
                const int alpha = static_cast<int>(lightning * 54.0f);
                background->AddRectFilled(
                    ImVec2(0.0f, 0.0f),
                    ImVec2(width, height),
                    IM_COL32(205, 220, 238, alpha));

                const float boltX = width * (0.18f + hash01(77u + static_cast<std::uint32_t>(totalTime / 8.7f)) * 0.64f);
                std::array<ImVec2, 6> bolt{
                    ImVec2(boltX, 0.0f),
                    ImVec2(boltX - 18.0f, height * 0.12f),
                    ImVec2(boltX + 10.0f, height * 0.20f),
                    ImVec2(boltX - 26.0f, height * 0.31f),
                    ImVec2(boltX + 2.0f, height * 0.41f),
                    ImVec2(boltX - 14.0f, height * 0.52f),
                };
                background->AddPolyline(
                    bolt.data(),
                    static_cast<int>(bolt.size()),
                    IM_COL32(225, 235, 255, static_cast<int>(lightning * 148.0f)),
                    0,
                    2.0f);
            }

            constexpr std::uint32_t kDropCount = 560;
            for (std::uint32_t i = 0; i < kDropCount; ++i)
            {
                const float seedX = hash01(i * 1973u + 17u);
                const float seedY = hash01(i * 9277u + 71u);
                const float seedSpeed = hash01(i * 3181u + 131u);
                const float fall = wrap01(seedY + totalTime * (1.45f + seedSpeed * 0.55f));
                const float drift = totalTime * (85.0f + seedSpeed * 40.0f);
                const float x = std::fmod(seedX * (width + 180.0f) + drift, width + 180.0f) - 90.0f;
                const float y = fall * (height + 120.0f) - 80.0f;
                const float length = 24.0f + seedSpeed * 24.0f;
                const float slant = -10.0f - seedSpeed * 10.0f;
                const auto color = IM_COL32(185, 205, 225, static_cast<int>(92 + seedSpeed * 70.0f));
                drawList->AddLine(ImVec2(x, y), ImVec2(x + slant, y + length), color, 1.0f);
            }

            constexpr std::uint32_t kSplashCount = 96;
            for (std::uint32_t i = 0; i < kSplashCount; ++i)
            {
                const float rx = hash01(i * 1451u + 19u) - 0.5f;
                const float rz = hash01(i * 2819u + 47u) - 0.5f;
                const float phase = wrap01(hash01(i * 577u + 83u) + totalTime * (1.7f + hash01(i * 1297u) * 0.8f));
                if (phase > 0.42f)
                    continue;

                ImVec2 screen{};
                const float splashX = view.x + rx * 130.0f;
                const float splashZ = view.z + rz * 130.0f;
                if (!project_world_to_screen(view, splashX, kWeatherWaterY + 0.03f, splashZ, width, height, screen))
                    continue;

                const float alpha = (1.0f - phase / 0.42f) * 105.0f;
                const float radius = 2.0f + phase * 10.0f;
                drawList->AddCircle(
                    screen,
                    radius,
                    IM_COL32(210, 230, 240, static_cast<int>(alpha)),
                    16,
                    1.0f);
            }
            return;
        }

        if (weatherMode == WeatherMode::Sunset)
        {
            background->AddRectFilled(
                ImVec2(0.0f, 0.0f),
                ImVec2(width, height),
                IM_COL32(255, 128, 54, 18));
            return;
        }

        if (weatherMode == WeatherMode::Night)
        {
            background->AddRectFilled(
                ImVec2(0.0f, 0.0f),
                ImVec2(width, height),
                IM_COL32(2, 6, 18, 70));
            return;
        }

        background->AddRectFilled(
            ImVec2(0.0f, 0.0f),
            ImVec2(width, height),
            IM_COL32(210, 215, 220, 16));

        constexpr std::uint32_t kFlakeCount = 360;
        for (std::uint32_t i = 0; i < kFlakeCount; ++i)
        {
            const float seedX = hash01(i * 1789u + 11u);
            const float seedY = hash01(i * 3259u + 29u);
            const float seedSize = hash01(i * 811u + 101u);
            const float speed = 0.22f + seedSize * 0.34f;
            const float fall = wrap01(seedY + totalTime * speed);
            const float sway = std::sin(totalTime * (1.2f + seedSize) + seedX * 16.0f) * (12.0f + seedSize * 28.0f);
            const float x = wrap01(seedX + totalTime * 0.018f) * (width + 80.0f) - 40.0f + sway;
            const float y = fall * (height + 70.0f) - 35.0f;
            const float radius = 1.1f + seedSize * 2.1f;
            const auto alpha = static_cast<int>(120.0f + seedSize * 95.0f);
            drawList->AddCircleFilled(ImVec2(x, y), radius, IM_COL32(245, 248, 255, alpha), 8);
        }
    }

    struct LoadingIcon
    {
        std::uint32_t width{};
        std::uint32_t height{};
        const std::uint8_t* bgra{};
        bool valid() const { return width > 0 && height > 0 && bgra != nullptr; }
    };

    void blit_icon(
        std::vector<std::uint8_t>& pixels,
        std::uint32_t width,
        std::uint32_t height,
        const LoadingIcon& icon,
        std::uint32_t dstX,
        std::uint32_t dstY,
        std::uint32_t dstSize)
    {
        if (!icon.valid() || dstSize == 0)
            return;
        for (std::uint32_t y = 0; y < dstSize; ++y)
        {
            const auto py = dstY + y;
            if (py >= height)
                continue;
            const auto sy = std::min(icon.height - 1u, y * icon.height / dstSize);
            for (std::uint32_t x = 0; x < dstSize; ++x)
            {
                const auto px = dstX + x;
                if (px >= width)
                    continue;
                const auto sx = std::min(icon.width - 1u, x * icon.width / dstSize);
                const auto src = (static_cast<std::size_t>(sy) * icon.width + sx) * 4u;
                const auto dst = (static_cast<std::size_t>(py) * width + px) * 4u;
                const float alpha = static_cast<float>(icon.bgra[src + 3]) / 255.0f;
                for (std::size_t c = 0; c < 3; ++c)
                {
                    pixels[dst + c] = static_cast<std::uint8_t>(
                        static_cast<float>(icon.bgra[src + c]) * alpha
                        + static_cast<float>(pixels[dst + c]) * (1.0f - alpha));
                }
                pixels[dst + 3] = 255;
            }
        }
    }

    void fill_rect(
        std::vector<std::uint8_t>& pixels,
        std::uint32_t width,
        std::uint32_t height,
        std::uint32_t x0,
        std::uint32_t y0,
        std::uint32_t x1,
        std::uint32_t y1,
        std::uint8_t b,
        std::uint8_t g,
        std::uint8_t r,
        std::uint8_t a = 255)
    {
        x0 = std::min(x0, width);
        x1 = std::min(x1, width);
        y0 = std::min(y0, height);
        y1 = std::min(y1, height);
        for (std::uint32_t y = y0; y < y1; ++y)
        {
            for (std::uint32_t x = x0; x < x1; ++x)
            {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4u;
                pixels[offset + 0] = b;
                pixels[offset + 1] = g;
                pixels[offset + 2] = r;
                pixels[offset + 3] = a;
            }
        }
    }

    std::array<std::uint8_t, 7> glyph_rows(char ch)
    {
        switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch))))
        {
        case 'A': return {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        case 'D': return {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e};
        case 'E': return {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f};
        case 'G': return {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e};
        case 'H': return {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11};
        case 'I': return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f};
        case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f};
        case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
        case 'O': return {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e};
        case 'T': return {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
        default: return {0, 0, 0, 0, 0, 0, 0};
        }
    }

    std::uint32_t text_pixel_width(std::string_view text, std::uint32_t scale)
    {
        if (text.empty())
            return 0;
        return static_cast<std::uint32_t>(text.size()) * 6u * scale - scale;
    }

    void draw_text(
        std::vector<std::uint8_t>& pixels,
        std::uint32_t width,
        std::uint32_t height,
        std::string_view text,
        std::uint32_t startX,
        std::uint32_t startY,
        std::uint32_t scale,
        std::uint8_t b,
        std::uint8_t g,
        std::uint8_t r)
    {
        if (scale == 0)
            return;
        auto cursorX = startX;
        for (const char ch : text)
        {
            const auto rows = glyph_rows(ch);
            for (std::uint32_t row = 0; row < rows.size(); ++row)
            {
                for (std::uint32_t col = 0; col < 5u; ++col)
                {
                    if ((rows[row] & (1u << (4u - col))) == 0)
                        continue;
                    fill_rect(
                        pixels,
                        width,
                        height,
                        cursorX + col * scale,
                        startY + row * scale,
                        cursorX + (col + 1u) * scale,
                        startY + (row + 1u) * scale,
                        b,
                        g,
                        r,
                        255);
                }
            }
            cursorX += 6u * scale;
        }
    }

    std::vector<std::uint8_t> make_loading_image(
        std::uint32_t width,
        std::uint32_t height,
        float progress,
        const LoadingIcon& icon)
    {
        width = std::max(1u, width);
        height = std::max(1u, height);
        progress = std::clamp(progress, 0.0f, 1.0f);

        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const float v = static_cast<float>(y) / static_cast<float>(std::max(1u, height - 1u));
            const std::uint8_t r = static_cast<std::uint8_t>(8.0f + v * 12.0f);
            const std::uint8_t g = static_cast<std::uint8_t>(12.0f + v * 18.0f);
            const std::uint8_t b = static_cast<std::uint8_t>(18.0f + v * 32.0f);
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const auto offset = (static_cast<std::size_t>(y) * width + x) * 4u;
                pixels[offset + 0] = b;
                pixels[offset + 1] = g;
                pixels[offset + 2] = r;
                pixels[offset + 3] = 255;
            }
        }

        const auto barWidth = std::max(240u, width / 3u);
        const auto barHeight = std::max(10u, height / 90u);
        const auto barX = (width - barWidth) / 2u;
        const auto barY = height / 2u + height / 10u;
        const auto iconSize = std::clamp(height / 7u, 72u, 132u);
        const auto iconX = (width - iconSize) / 2u;
        const auto iconY = barY > iconSize + 92u ? barY - iconSize - 92u : height / 8u;
        blit_icon(pixels, width, height, icon, iconX, iconY, iconSize);

        constexpr std::string_view label = "Loading the engine";
        const auto textScale = std::clamp(height / 360u, 2u, 4u);
        const auto textWidth = text_pixel_width(label, textScale);
        const auto textX = width > textWidth ? (width - textWidth) / 2u : 0u;
        const auto textY = barY > 52u ? barY - 52u : barY;
        draw_text(pixels, width, height, label, textX + 1u, textY + 1u, textScale, 20, 24, 30);
        draw_text(pixels, width, height, label, textX, textY, textScale, 238, 242, 248);

        fill_rect(pixels, width, height, barX - 2u, barY - 2u, barX + barWidth + 2u, barY + barHeight + 2u, 32, 40, 52);
        fill_rect(pixels, width, height, barX, barY, barX + barWidth, barY + barHeight, 18, 23, 31);
        fill_rect(
            pixels,
            width,
            height,
            barX,
            barY,
            barX + static_cast<std::uint32_t>(static_cast<float>(barWidth) * progress),
            barY + barHeight,
            96,
            158,
            236);
        return pixels;
    }
    std::vector<phoenix::renderer::TerrainDrawRange> build_visible_terrain_ranges(
        const phoenix::runtime::PhoenixRuntime& runtime,
        const CameraView& view)
    {
        std::vector<phoenix::renderer::TerrainDrawRange> ranges;
        const auto& world = runtime.state().world;
        if (!world.parsed || world.heightMapSide < 2 || world.isDungeon)
            return ranges;

        const auto mapSize = static_cast<float>(std::max(1u, world.mapSize));
        const auto halfMap = mapSize * 0.5f;
        const auto grid = world.heightMapSide - 1;
        const auto cellSize = mapSize / static_cast<float>(grid);
        constexpr std::uint32_t kChunkQuads = 16;
        const auto chunkCount = (grid + kChunkQuads - 1u) / kChunkQuads;

        ranges.reserve(static_cast<std::size_t>(chunkCount) * chunkCount);
        for (std::uint32_t cz = 0; cz < chunkCount; ++cz)
        {
            for (std::uint32_t cx = 0; cx < chunkCount; ++cx)
            {
                const auto minX = cx * kChunkQuads;
                const auto minZ = cz * kChunkQuads;
                const auto maxX = std::min(grid, minX + kChunkQuads);
                const auto maxZ = std::min(grid, minZ + kChunkQuads);
                const auto centerX = -halfMap + (static_cast<float>(minX + maxX) * 0.5f) * cellSize;
                const auto centerZ = -halfMap + (static_cast<float>(minZ + maxZ) * 0.5f) * cellSize;
                const auto extentX = static_cast<float>(maxX - minX) * cellSize * 0.5f;
                const auto extentZ = static_cast<float>(maxZ - minZ) * cellSize * 0.5f;
                const auto radius = std::sqrt(extentX * extentX + extentZ * extentZ) + 180.0f;
                if (!sphere_visible(view, centerX, 30.0f, centerZ, radius))
                    continue;

                for (std::uint32_t z = minZ; z < maxZ; ++z)
                {
                    phoenix::renderer::TerrainDrawRange range{};
                    range.firstIndex = (z * grid + minX) * 6u;
                    range.indexCount = (maxX - minX) * 6u;
                    ranges.push_back(range);
                }
            }
        }

        return ranges;
    }

    std::vector<phoenix::renderer::BatchBoundsGpu> extract_gpu_bounds(
        const phoenix::runtime::StaticObjectScene& scene)
    {
        std::vector<phoenix::renderer::BatchBoundsGpu> gpuBounds(scene.batchBounds.size());
        for (std::size_t i = 0; i < scene.batchBounds.size(); ++i)
        {
            gpuBounds[i].x = scene.batchBounds[i].x;
            gpuBounds[i].y = scene.batchBounds[i].y;
            gpuBounds[i].z = scene.batchBounds[i].z;
            gpuBounds[i].radius = scene.batchBounds[i].radius;
        }
        return gpuBounds;
    }

    void build_visible_object_batches(
        const phoenix::runtime::StaticObjectScene& scene,
        const CameraView& view,
        std::size_t actorBatchStart,
        float actorViewDistance,
        std::vector<phoenix::renderer::ObjectBatch>& batches)
    {
        batches.clear();
        if (scene.batches.empty() || scene.batchBounds.size() != scene.batches.size())
            return;

        const auto limit = std::min(scene.batches.size(), actorViewDistance < 0.0f ? actorBatchStart : scene.batches.size());
        batches.reserve(limit);
        for (std::size_t i = 0; i < scene.batches.size(); ++i)
        {
            if (i >= actorBatchStart && actorViewDistance < 0.0f)
                break;
            const auto& bounds = scene.batchBounds[i];
            auto batchView = view;
            if (i >= actorBatchStart)
                batchView.distance = std::min(batchView.distance, actorViewDistance);
            if (!sphere_visible(batchView, bounds.x, bounds.y, bounds.z, bounds.radius))
                continue;

            batches.push_back(scene.batches[i]);
        }
    }

    void build_visible_animated_batches(
        const phoenix::runtime::AnimatedObjectScene& scene,
        const CameraView& view,
        std::size_t actorAnimatedBatchStart,
        float actorViewDistance,
        std::vector<phoenix::renderer::ObjectBatch>& batches)
    {
        batches.clear();
        if (scene.batches.empty() || scene.batchBounds.size() != scene.batches.size())
            return;

        batches.reserve(scene.batches.size());
        for (std::size_t i = 0; i < scene.batches.size(); ++i)
        {
            if (i >= actorAnimatedBatchStart && actorViewDistance < 0.0f)
                break;
            const auto& bounds = scene.batchBounds[i];
            auto batchView = view;
            if (i >= actorAnimatedBatchStart)
                batchView.distance = std::min(batchView.distance, actorViewDistance);
            if (sphere_visible(batchView, bounds.x, bounds.y, bounds.z, bounds.radius))
                batches.push_back(scene.batches[i]);
        }
    }

    // ====================================================================
    // Actor spatial grid: divide map into cells, stream nearby actors.
    // ====================================================================
    struct ActorGrid
    {
        static constexpr float kCellSize = 96.0f;

        static int to_cell(float v) { return static_cast<int>(std::floor(v / kCellSize)); }
        static std::uint64_t cell_key(int cx, int cz)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
                 | static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
        }

        struct StaticTemplate { std::uint32_t firstIndex; std::uint32_t indexCount; };
        struct StaticEntry { std::uint32_t tmpl; phoenix::renderer::ObjectInstance inst; };

        std::vector<StaticTemplate> staticTemplates;
        std::unordered_map<std::uint64_t, std::vector<StaticEntry>> staticCells;

        std::vector<phoenix::renderer::ObjectInstance> worldInstances;
        std::vector<phoenix::renderer::ObjectBatch> worldBatches;
        std::vector<phoenix::runtime::StaticObjectScene::BatchBounds> worldBounds;

        int lastCX = INT_MAX;
        int lastCZ = INT_MAX;
        bool built{};

        void reset()
        {
            staticTemplates.clear();
            staticCells.clear();
            worldInstances.clear();
            worldBatches.clear();
            worldBounds.clear();
            lastCX = INT_MAX;
            lastCZ = INT_MAX;
            built = false;
        }

        bool camera_cell_changed(float cx, float cz)
        {
            const int x = to_cell(cx);
            const int z = to_cell(cz);
            if (x == lastCX && z == lastCZ)
                return false;
            lastCX = x;
            lastCZ = z;
            return true;
        }

        void rebuild(float cameraX, float cameraZ, float actorViewDist,
            phoenix::runtime::StaticObjectScene& scene,
            std::size_t& outActorBatchStart) const
        {
            scene.instances = worldInstances;
            scene.batches = worldBatches;
            scene.batchBounds = worldBounds;
            outActorBatchStart = scene.batches.size();
            if (staticCells.empty())
                return;

            const int cx0 = to_cell(cameraX);
            const int cz0 = to_cell(cameraZ);
            const int r = std::max(1, static_cast<int>(std::ceil(actorViewDist / kCellSize)));

            std::unordered_map<std::uint32_t, std::vector<const phoenix::renderer::ObjectInstance*>> groups;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx)
                {
                    auto it = staticCells.find(cell_key(cx0 + dx, cz0 + dz));
                    if (it == staticCells.end()) continue;
                    for (const auto& e : it->second)
                        groups[e.tmpl].push_back(&e.inst);
                }

            for (const auto& [ti, insts] : groups)
            {
                const auto& t = staticTemplates[ti];
                phoenix::renderer::ObjectBatch batch{};
                batch.firstIndex = t.firstIndex;
                batch.indexCount = t.indexCount;
                batch.firstInstance = static_cast<std::uint32_t>(scene.instances.size());
                batch.instanceCount = static_cast<std::uint32_t>(insts.size());

                float minX = std::numeric_limits<float>::max(), minY = minX, minZ = minX;
                float maxX = std::numeric_limits<float>::lowest(), maxY = maxX, maxZ = maxX;
                for (const auto* inst : insts)
                {
                    scene.instances.push_back(*inst);
                    minX = std::min(minX, inst->position[0]);
                    minY = std::min(minY, inst->position[1]);
                    minZ = std::min(minZ, inst->position[2]);
                    maxX = std::max(maxX, inst->position[0]);
                    maxY = std::max(maxY, inst->position[1]);
                    maxZ = std::max(maxZ, inst->position[2]);
                }
                phoenix::runtime::StaticObjectScene::BatchBounds bb{};
                bb.x = (minX + maxX) * 0.5f;
                bb.y = (minY + maxY) * 0.5f;
                bb.z = (minZ + maxZ) * 0.5f;
                const auto hx = (maxX - minX) * 0.5f + 8.0f;
                const auto hy = (maxY - minY) * 0.5f + 8.0f;
                const auto hz = (maxZ - minZ) * 0.5f + 8.0f;
                bb.radius = std::sqrt(hx * hx + hy * hy + hz * hz);
                scene.batches.push_back(batch);
                scene.batchBounds.push_back(bb);
            }
        }
    };

    void split_animated_actor_batches_by_cell(
        phoenix::runtime::AnimatedObjectScene& scene,
        phoenix::world::ActorScene& actorScene,
        std::size_t actorAnimBatchStart,
        std::size_t instanceBase)
    {
        if (actorAnimBatchStart >= scene.batches.size())
            return;

        struct CellGroup
        {
            std::size_t origBatch;
            std::vector<std::uint32_t> oldIndices;
        };
        std::vector<CellGroup> groups;

        for (std::size_t b = actorAnimBatchStart; b < scene.batches.size(); ++b)
        {
            const auto& batch = scene.batches[b];
            std::map<std::uint64_t, std::size_t> cellMap;
            for (std::uint32_t j = 0; j < batch.instanceCount; ++j)
            {
                const auto gi = batch.firstInstance + j;
                if (gi >= scene.instances.size()) continue;
                const auto& inst = scene.instances[gi];
                const auto key = ActorGrid::cell_key(
                    ActorGrid::to_cell(inst.position[0]),
                    ActorGrid::to_cell(inst.position[2]));
                auto it = cellMap.find(key);
                if (it == cellMap.end())
                {
                    it = cellMap.emplace(key, groups.size()).first;
                    groups.push_back({ b, {} });
                }
                groups[it->second].oldIndices.push_back(gi);
            }
        }
        if (groups.empty()) return;

        std::unordered_map<std::uint32_t, std::uint32_t> remap;
        std::vector<phoenix::renderer::ObjectInstance> newInst(
            scene.instances.begin(), scene.instances.begin() + instanceBase);
        std::vector<phoenix::renderer::ObjectInstance> newBase(
            scene.baseInstances.begin(), scene.baseInstances.begin() + instanceBase);
        newInst.reserve(scene.instances.size());
        newBase.reserve(scene.baseInstances.size());

        std::vector<phoenix::renderer::ObjectBatch> newBatches;
        std::vector<phoenix::runtime::StaticObjectScene::BatchBounds> newBounds;

        for (const auto& g : groups)
        {
            const auto& orig = scene.batches[g.origBatch];
            phoenix::renderer::ObjectBatch sb{};
            sb.firstIndex = orig.firstIndex;
            sb.indexCount = orig.indexCount;
            sb.firstInstance = static_cast<std::uint32_t>(newInst.size());
            sb.instanceCount = static_cast<std::uint32_t>(g.oldIndices.size());

            float minX = std::numeric_limits<float>::max(), minY = minX, minZ = minX;
            float maxX = std::numeric_limits<float>::lowest(), maxY = maxX, maxZ = maxX;
            for (auto oi : g.oldIndices)
            {
                remap[oi] = static_cast<std::uint32_t>(newInst.size());
                newInst.push_back(scene.instances[oi]);
                if (oi < scene.baseInstances.size())
                    newBase.push_back(scene.baseInstances[oi]);
                const auto& p = scene.instances[oi].position;
                minX = std::min(minX, p[0] - 6.0f); maxX = std::max(maxX, p[0] + 6.0f);
                minY = std::min(minY, p[1]);         maxY = std::max(maxY, p[1] + 12.0f);
                minZ = std::min(minZ, p[2] - 6.0f); maxZ = std::max(maxZ, p[2] + 6.0f);
            }
            phoenix::runtime::StaticObjectScene::BatchBounds bb{};
            bb.x = (minX + maxX) * 0.5f;
            bb.y = (minY + maxY) * 0.5f;
            bb.z = (minZ + maxZ) * 0.5f;
            const auto hx = (maxX - minX) * 0.5f;
            const auto hy = (maxY - minY) * 0.5f;
            const auto hz = (maxZ - minZ) * 0.5f;
            bb.radius = std::sqrt(hx * hx + hy * hy + hz * hz);
            newBatches.push_back(sb);
            newBounds.push_back(bb);
        }

        scene.instances = std::move(newInst);
        scene.baseInstances = std::move(newBase);
        scene.batches.resize(actorAnimBatchStart);
        scene.batchBounds.resize(actorAnimBatchStart);
        scene.batches.insert(scene.batches.end(), newBatches.begin(), newBatches.end());
        scene.batchBounds.insert(scene.batchBounds.end(), newBounds.begin(), newBounds.end());

        for (auto& label : actorScene.labels)
        {
            if (!label.followsAnimatedInstance || label.animatedInstanceIndex == UINT32_MAX)
                continue;
            auto it = remap.find(label.animatedInstanceIndex);
            if (it != remap.end())
                label.animatedInstanceIndex = it->second;
        }
        for (auto& mob : scene.mobInstances)
        {
            auto it = remap.find(mob.instanceIndex);
            if (it != remap.end())
                mob.instanceIndex = it->second;
        }
    }

}

int main(int, char**)
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    const auto executableDir = executable_directory();
    phoenix::core::initialize_logging(executableDir);
    {
        std::ofstream log = phoenix::core::open_engine_log();
        log << "Phoenix Engine\n";
        log << "Executable directory: " << executableDir.string() << "\n";
    }

    phoenix::platform::SdlWindow window;
    if (!window.create(kWidth, kHeight, kAppTitle))
    {
        std::fprintf(stderr, "Could not create Phoenix Engine window.\n");
        return 1;
    }

    const auto [clientWidth, clientHeight] = window.client_size();
    phoenix::renderer::VulkanRenderer renderer;
    if (!renderer.initialize(
        window.handle(),
        static_cast<std::uint32_t>(clientWidth > 0 ? clientWidth : kWidth),
        static_cast<std::uint32_t>(clientHeight > 0 ? clientHeight : kHeight)))
    {
        std::fprintf(stderr, "Could not initialize Vulkan.\n");
        return 1;
    }

    const LoadingIcon loadingIcon{
        phoenix::generated::kLoadingIconWidth,
        phoenix::generated::kLoadingIconHeight,
        phoenix::generated::kLoadingIconBgra};

    const auto imguiAvailable = renderer.initialize_imgui(window.handle());
    if (!imguiAvailable)
    {
        std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
        log << "ImGui initialization unavailable\n";
    }

    std::string lastLoggedLoadingStage;
    auto showLoading = [&](float progress, std::string_view stage) {
        if (lastLoggedLoadingStage != stage)
        {
            lastLoggedLoadingStage = std::string(stage);
            phoenix::core::write_log_line("Loading", lastLoggedLoadingStage);
        }
        window.pump_messages();
        window.set_title(std::string(kAppTitle) + " - Loading - " + std::string(stage));
        const auto [sw, sh] = window.client_size();
        if (sw > 0 && sh > 0)
        {
            const auto width = static_cast<std::uint32_t>(sw);
            const auto height = static_cast<std::uint32_t>(sh);
            renderer.resize(width, height);
            auto image = make_loading_image(width, height, progress, loadingIcon);
            renderer.set_preview_image(width, height, image);
            renderer.render_frame();
        }
    };

    // Run a function on a background thread while keeping the window responsive.
    auto runAsync = [&](auto fn, float progress, std::string_view stage) {
        auto future = std::async(std::launch::async, std::move(fn));
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
            showLoading(progress, stage);
        return future.get();
    };

    // Overload for void-returning functions.
    auto runAsyncVoid = [&](auto fn, float progress, std::string_view stage) {
        auto future = std::async(std::launch::async, std::move(fn));
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
            showLoading(progress, stage);
        future.get();
    };

    showLoading(0.03f, "Starting");

    phoenix::runtime::PhoenixRuntime runtime;
    runAsyncVoid([&]() { runtime.initialize(executableDir, false); }, 0.06f, "Indexing data");
    showLoading(0.12f, "Indexing data");

    // Character system + its renderer-independent catalog preload. Kicked off here
    // so the heavy model/texture (BC3) cache build overlaps the world load instead
    // of running serially after it. Joined just before the character is first used.
    phoenix::character::CharacterSystem characterSystem;
    phoenix::character::WeaponEffect weaponEffect;
    phoenix::character::CharacterAppearance characterAppearance{};
    const auto charDataRoot = runtime.state().assets.root;
    auto charPreloadFuture = std::async(std::launch::async, [&characterSystem, charDataRoot]() {
        characterSystem.preload(charDataRoot);
        characterSystem.preload_items(charDataRoot);
    });

    std::size_t defaultMap{};
    const auto& startupMaps = runtime.world_map_names();
    for (std::size_t i = 0; i < startupMaps.size(); ++i)
    {
        if (phoenix::assets::lower_ascii(startupMaps[i]) == "1.wld")
        {
            defaultMap = i;
            break;
        }
    }
    if (!startupMaps.empty())
        runAsyncVoid([&]() { runtime.load_world_map(defaultMap); }, 0.17f, "Loading world");
    showLoading(0.24f, "World ready");

    phoenix::audio::AudioSystem audioSystem;
    const bool audioAvailable = audioSystem.initialize();
    showLoading(0.27f, "Audio");
    if (!audioAvailable)
    {
        std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
        log << "Audio initialization unavailable\n";
    }

    auto characterOptions = scan_character_options(runtime.state().assets.root);
    // Join the background character-catalog preload started before the world load.
    // It typically finished during the (longer) world load, so this rarely blocks.
    while (charPreloadFuture.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
        showLoading(0.30f, "Loading characters");
    charPreloadFuture.get();
    showLoading(0.32f, "Characters");
    int selectedCharacterOption = 0;
    for (std::size_t i = 0; i < characterOptions.size(); ++i)
    {
        if (characterOptions[i].raceFolder == characterAppearance.raceFolder
            && characterOptions[i].prefix == characterAppearance.prefix)
        {
            selectedCharacterOption = static_cast<int>(i);
            break;
        }
    }
    bool characterLoaded = false;
    bool playableMode = true; // true = third-person playable, false = free camera viewer

    bool fogEnabled = true;
    float viewDistance = 1000.0f;
    float actorViewDistance = 100.0f;
    bool actorsEnabled = true;
    bool showNamePlates = true;
    bool showCollisionDebug = false;
    WeatherMode weatherMode = WeatherMode::Default;
    const auto applyFogSettings = [&]() {
        apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);
    };
    applyFogSettings();

    std::uint32_t terrainVertexCount{};
    std::uint32_t terrainIndexCount{};
    std::uint32_t objectInstanceCount{};
    std::uint32_t objectBatchCount{};
    bool showSoundGizmos = false;
    bool showMusicGizmos = false;
    bool showPortalGizmos = false;
    bool showEffectGizmos = false;
    bool playMapSounds = true;
    bool playMapMusic = true;
    int selectedMapIndex = static_cast<int>(runtime.selected_world_map());
    bool terrainTexturesUploaded = false;
    std::size_t characterTextureBaseSlot = 0;
    phoenix::runtime::StaticObjectScene staticObjectScene;
    phoenix::runtime::AnimatedObjectScene animatedObjectScene;
    phoenix::world::ActorScene actorScene;
    ActorGrid actorGrid;
    std::size_t actorBatchStart = std::numeric_limits<std::size_t>::max();
    std::size_t actorAnimatedBatchStart = std::numeric_limits<std::size_t>::max();
    std::size_t actorVertexAnimationStart = std::numeric_limits<std::size_t>::max();
    phoenix::runtime::WorldCollisionMesh worldCollisionMesh;
    HeightSamplerContext heightSamplerCtx{ &runtime, &worldCollisionMesh };
    MapAudioScene mapAudioScene;
    bool forceVisibilityUpdate = true;
    bool gpuSkinningActive = false;
    std::optional<std::size_t> pendingMapLoad;
    CameraView lastCullView{};
    float displayedFps = 0.0f;
    PerfHudState perfHud;
    perfHud.initialize_system_info();
    perfHud.gpuName = renderer.adapter_name();
    perfHud.renderer = &renderer;

    const auto uploadDebugGizmos = [&]() {
        std::vector<phoenix::renderer::TerrainVertex> debugVertices;
        std::vector<std::uint32_t> debugIndices;
        runtime.build_debug_gizmo_mesh(
            showSoundGizmos,
            showMusicGizmos,
            showPortalGizmos,
            showEffectGizmos,
            debugVertices,
            debugIndices);

        // Append collision mesh visualization.
        if (showCollisionDebug && !worldCollisionMesh.triangles.empty())
        {
            for (const auto& tri : worldCollisionMesh.triangles)
            {
                const auto base = static_cast<std::uint32_t>(debugVertices.size());
                phoenix::renderer::TerrainVertex v{};
                v.color[0] = 0.0f; v.color[1] = 1.0f; v.color[2] = 0.3f;
                v.normal[1] = 1.0f;
                v.textureLayer = 0xFFFFFFFFu;

                v.position[0] = tri.v0[0]; v.position[1] = tri.v0[1]; v.position[2] = tri.v0[2];
                debugVertices.push_back(v);
                v.position[0] = tri.v1[0]; v.position[1] = tri.v1[1]; v.position[2] = tri.v1[2];
                debugVertices.push_back(v);
                v.position[0] = tri.v2[0]; v.position[1] = tri.v2[1]; v.position[2] = tri.v2[2];
                debugVertices.push_back(v);

                debugIndices.push_back(base + 0);
                debugIndices.push_back(base + 1);
                debugIndices.push_back(base + 2);
            }
        }

        renderer.set_debug_mesh(debugVertices, debugIndices);
        renderer.set_debug_visible(showSoundGizmos || showMusicGizmos || showPortalGizmos || showEffectGizmos || showCollisionDebug);
    };

    const auto uploadCharacterMesh = [&]() {
        if (!characterLoaded || !characterSystem.ready())
            return;

        phoenix::character::PlayableInput noInput{};
        characterSystem.update(0.0f, noInput);
        const auto& charVerts = characterSystem.world_vertices();
        const auto& charIndices = characterSystem.indices();
        static_assert(sizeof(phoenix::character::CharacterGpuVertex) == sizeof(phoenix::renderer::TerrainVertex),
            "CharacterGpuVertex must match TerrainVertex layout");
        const auto* terrainVerts = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(charVerts.data());
        std::vector<phoenix::renderer::TerrainVertex> tv(terrainVerts, terrainVerts + charVerts.size());
        renderer.set_character_mesh(tv, charIndices);
        renderer.set_character_visible(playableMode);
    };

    const auto releaseDecodedTextureRam = [](std::vector<phoenix::renderer::DdsTexture>& textures) {
        for (auto& texture : textures)
        {
            std::vector<std::uint8_t>().swap(texture.rgba);
        }
    };

    // Normalise all textures to uniform BC3 format at the dominant resolution
    // so they can be uploaded as a single GPU-compressed Texture2DArray.
    const auto normalizeTexturesForBcUpload = [](std::vector<phoenix::renderer::DdsTexture>& textures) {
        if (textures.empty())
            return;

        // Vote on the dominant resolution among valid textures.
        std::map<std::uint64_t, std::uint32_t> sizeCounts;
        for (const auto& t : textures)
        {
            if (!t.valid || t.width == 0 || t.height == 0) continue;
            const auto key = (static_cast<std::uint64_t>(t.width) << 32) | t.height;
            sizeCounts[key]++;
        }
        std::uint32_t targetW = 256, targetH = 256; // default
        std::uint32_t bestCount = 0;
        for (const auto& [key, count] : sizeCounts)
        {
            if (count > bestCount)
            {
                bestCount = count;
                targetW = static_cast<std::uint32_t>(key >> 32);
                targetH = static_cast<std::uint32_t>(key & 0xFFFFFFFF);
            }
        }

        // Compute mip count: down to 4×4 blocks (same as renderer logic).
        const auto maxDim = std::max(targetW, targetH);
        const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
        const auto targetMips = std::min(fullMips,
            static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)));

        // Convert each texture to BC3 in parallel.
        std::atomic<std::size_t> nextTex{ 0 };
        const auto workerCount = std::min(
            static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
            textures.size());
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t w = 0; w < workerCount; ++w)
        {
            workers.emplace_back([&]() {
                for (;;)
                {
                    const auto idx = nextTex.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= textures.size()) break;
                    phoenix::renderer::convert_texture_to_bc3(
                        textures[idx], targetW, targetH, targetMips);
                }
            });
        }
        for (auto& w : workers) w.join();
    };

    constexpr std::size_t kCharacterTextureSlotReserve = 32;

    const auto reloadCharacterIntoRenderer = [&]() {
        if (characterTextureBaseSlot == 0 || !terrainTexturesUploaded)
            return false;

        characterLoaded = characterSystem.load(runtime.state().assets.root, characterAppearance);
        if (!characterLoaded)
            return false;

        characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
        characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
        const auto& charTexPaths = characterSystem.texture_paths();
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));

        // Fast path: lookup pre-cached BC3 textures (no disk I/O, no conversion).
        if (characterSystem.bc3_cache_ready() && charTexPaths.size() <= kCharacterTextureSlotReserve)
        {
            std::vector<phoenix::renderer::DdsTexture> characterTextures(charTexPaths.size());
            bool allCached = true;
            for (std::size_t i = 0; i < charTexPaths.size(); ++i)
            {
                const auto* cached = characterSystem.bc3_texture_for(charTexPaths[i]);
                if (cached)
                    characterTextures[i] = *cached; // copy BC3 mip data from RAM cache
                else
                {
                    allCached = false;
                    break;
                }
            }
            if (!allCached)
            {
                // Fallback: load from disk + convert (should be rare).
                for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                    characterTextures[i] = phoenix::renderer::load_dds(charTexPaths[i]);
                normalizeTexturesForBcUpload(characterTextures);
            }
            renderer.upload_terrain_texture_layers(static_cast<std::uint32_t>(characterTextureBaseSlot), characterTextures);
        }
        else
        {
            // Fallback: load from disk + convert.
            std::vector<phoenix::renderer::DdsTexture> characterTextures(charTexPaths.size());
            for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                characterTextures[i] = phoenix::renderer::load_dds(charTexPaths[i]);
            normalizeTexturesForBcUpload(characterTextures);
            renderer.upload_terrain_texture_layers(static_cast<std::uint32_t>(characterTextureBaseSlot), characterTextures);
        }

        // Fast mesh update: reuses existing GPU buffers, no vkDeviceWaitIdle.
        if (characterSystem.ready())
        {
            phoenix::character::PlayableInput noInput{};
            characterSystem.update(0.0f, noInput);
            const auto& charVerts = characterSystem.world_vertices();
            const auto& charIndices = characterSystem.indices();
            const auto* terrainVerts = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(charVerts.data());
            std::vector<phoenix::renderer::TerrainVertex> tv(terrainVerts, terrainVerts + charVerts.size());
            renderer.update_character_mesh(tv, charIndices);
            renderer.set_character_visible(playableMode);
        }
        return true;
    };

    const auto uploadCurrentWorld = [&]() {
        renderer.enter_loading_mode();
        showLoading(0.36f, "Preparing scene");
        applyFogSettings();
        mapAudioScene = build_map_audio_scene(runtime);
        {
            std::ofstream audioLog(phoenix::core::engine_log_path(), std::ios::app);
            audioLog << "Map audio: musicZones=" << mapAudioScene.musicZones.size()
                << " soundEmitters=" << mapAudioScene.soundEmitters.size()
                << " available=" << (audioAvailable ? "yes" : "no") << "\n";
        }
        const auto texturePaths = runtime.terrain_texture_paths();
        const auto waterTexturePath = runtime.water_texture_path();
        const auto skyTexturePath = runtime.texture_path_for(runtime.state().world.skyFileName);
        const auto primaryCloudTexturePath = runtime.texture_path_for(runtime.state().world.primaryCloudFileName);
        const auto secondaryCloudTexturePath = runtime.texture_path_for(runtime.state().world.secondaryCloudFileName);
        const auto assetTexturePaths = runtime.asset_texture_paths();

        runtime.load_water_animation();
        const auto& waterAnim = runtime.water_animation();
        const auto waterFrameBase = kAssetTextureLayerBase + assetTexturePaths.size();
        const auto totalSlots = waterFrameBase + waterAnim.frameCount;
        const auto actorTextureBaseSlot = totalSlots;
        const auto mapStem = runtime.state().world.path.stem().string();
        actorScene = runAsync([&]() {
            return phoenix::world::build_actor_scene(
                runtime.state().dataRoot,
                mapStem,
                runtime.state().assets,
                static_cast<std::uint32_t>(actorTextureBaseSlot),
                character_height_sampler,
                &heightSamplerCtx);
        }, 0.38f, "Loading actors");
        characterTextureBaseSlot = actorTextureBaseSlot + actorScene.texturePaths.size();
        std::vector<phoenix::renderer::DdsTexture> terrainTextures(characterTextureBaseSlot);

        struct LoadJob
        {
            std::size_t slot;
            std::filesystem::path path;
        };
        std::vector<LoadJob> jobs;
        jobs.reserve(texturePaths.size() + 4 + assetTexturePaths.size() + waterAnim.frameCount + actorScene.texturePaths.size());
        for (std::size_t i = 0; i < texturePaths.size(); ++i)
            jobs.push_back({ i, texturePaths[i] });
        if (!waterTexturePath.empty())
            jobs.push_back({ kWaterTextureLayer, waterTexturePath });
        if (!skyTexturePath.empty())
            jobs.push_back({ kSkyTextureLayer, skyTexturePath });
        if (!primaryCloudTexturePath.empty())
            jobs.push_back({ kPrimaryCloudTextureLayer, primaryCloudTexturePath });
        if (!secondaryCloudTexturePath.empty())
            jobs.push_back({ kSecondaryCloudTextureLayer, secondaryCloudTexturePath });
        for (std::size_t i = 0; i < assetTexturePaths.size(); ++i)
            jobs.push_back({ kAssetTextureLayerBase + i, assetTexturePaths[i] });
        for (std::uint32_t i = 0; i < waterAnim.frameCount; ++i)
            jobs.push_back({ waterFrameBase + i, waterAnim.framePaths[i] });
        for (std::size_t i = 0; i < actorScene.texturePaths.size(); ++i)
            jobs.push_back({ actorTextureBaseSlot + i, actorScene.texturePaths[i] });

        const auto threadCount = std::min(
            static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
            jobs.size());
        if (!jobs.empty())
        {
            std::atomic<std::size_t> nextJob{ 0 };
            std::atomic<std::size_t> completedJobs{ 0 };
            auto worker = [&]() {
                for (;;)
                {
                    const auto idx = nextJob.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= jobs.size())
                        break;
                    terrainTextures[jobs[idx].slot] = phoenix::renderer::load_dds(jobs[idx].path);
                    completedJobs.fetch_add(1, std::memory_order_relaxed);
                }
            };
            std::vector<std::thread> threads;
            threads.reserve(threadCount);
            for (std::size_t t = 0; t < threadCount; ++t)
                threads.emplace_back(worker);
            while (completedJobs.load(std::memory_order_relaxed) < jobs.size())
            {
                const float textureProgress = static_cast<float>(completedJobs.load(std::memory_order_relaxed)) / static_cast<float>(jobs.size());
                showLoading(0.40f + textureProgress * 0.26f, "Loading textures");
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            for (auto& t : threads)
                t.join();
            showLoading(0.66f, "Textures ready");
        }


        {
            std::ofstream textureLog(phoenix::core::engine_log_path(), std::ios::app);
            textureLog << "Terrain texture layers: " << texturePaths.size()
                << " (loaded with " << threadCount << " threads)\n";
            for (std::size_t i = 0; i < texturePaths.size(); ++i)
            {
                const auto& tex = terrainTextures[i];
                textureLog << "  Layer " << i << ": " << texturePaths[i].string()
                    << " " << tex.width << "x" << tex.height
                    << (tex.valid ? " OK" : " FAILED") << "\n";
            }

            std::size_t loadedAssetTextures = 0;
            std::size_t failedAssetTextures = 0;
            textureLog << "Asset texture layers: " << assetTexturePaths.size()
                << " base=" << kAssetTextureLayerBase << "\n";
            for (std::size_t i = 0; i < assetTexturePaths.size(); ++i)
            {
                const auto& tex = terrainTextures[kAssetTextureLayerBase + i];
                if (tex.valid)
                    ++loadedAssetTextures;
                else
                    ++failedAssetTextures;

                if (i < kMaxLoggedAssetTextures || !tex.valid)
                {
                    textureLog << "  Asset layer " << (kAssetTextureLayerBase + i) << ": "
                        << assetTexturePaths[i].string()
                        << " " << tex.width << "x" << tex.height
                        << (tex.valid ? " OK" : " FAILED") << "\n";
                }
            }
            textureLog << "Asset texture summary: loaded=" << loadedAssetTextures
                << " failed=" << failedAssetTextures
                << " total=" << assetTexturePaths.size() << "\n";
        }

        const bool skyReady = !skyTexturePath.empty() && terrainTextures[kSkyTextureLayer].valid;
        const bool primaryCloudReady = !primaryCloudTexturePath.empty() && terrainTextures[kPrimaryCloudTextureLayer].valid;
        const bool secondaryCloudReady = !secondaryCloudTexturePath.empty() && terrainTextures[kSecondaryCloudTextureLayer].valid;
        renderer.set_sky_texture_layers(
            skyReady ? static_cast<std::uint32_t>(kSkyTextureLayer) : UINT32_MAX,
            primaryCloudReady ? static_cast<std::uint32_t>(kPrimaryCloudTextureLayer) : UINT32_MAX,
            secondaryCloudReady ? static_cast<std::uint32_t>(kSecondaryCloudTextureLayer) : UINT32_MAX);
        renderer.set_water_layer(
            !waterTexturePath.empty() ? static_cast<std::uint32_t>(kWaterTextureLayer) : UINT32_MAX);
        if (waterAnim.frameCount > 0)
            renderer.set_water_animation(
                static_cast<std::uint32_t>(waterFrameBase),
                waterAnim.frameCount,
                waterAnim.tileSize);

        runtime.set_effect_texture_base(0);

        // Load character textures into the texture array after water frames.
        // Reserve kCharacterTextureSlotReserve slots so appearance swaps don't resize the GPU array.
        {
            const auto reservedEnd = characterTextureBaseSlot + kCharacterTextureSlotReserve;
            terrainTextures.resize(reservedEnd); // fills reserved slots with empty (invalid) textures

            const auto loadCharTextures = [&]() {
                const auto& charTexPaths = characterSystem.texture_paths();
                // Use BC3 cache if available (instant), else load from disk.
                if (characterSystem.bc3_cache_ready())
                {
                    for (std::size_t i = 0; i < charTexPaths.size() && i < kCharacterTextureSlotReserve; ++i)
                    {
                        const auto* cached = characterSystem.bc3_texture_for(charTexPaths[i]);
                        if (cached)
                            terrainTextures[characterTextureBaseSlot + i] = *cached;
                        else
                            terrainTextures[characterTextureBaseSlot + i] = phoenix::renderer::load_dds(charTexPaths[i]);
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < charTexPaths.size() && i < kCharacterTextureSlotReserve; ++i)
                        terrainTextures[characterTextureBaseSlot + i] = phoenix::renderer::load_dds(charTexPaths[i]);
                }
                characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));
            };

            if (!characterLoaded)
            {
                characterLoaded = characterSystem.load(runtime.state().assets.root, characterAppearance);
                if (characterLoaded)
                {
                    characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
                    characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
                    loadCharTextures();

                    std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                    log << "Character textures base layer: " << characterTextureBaseSlot
                        << " reserved: " << kCharacterTextureSlotReserve
                        << " used: " << characterSystem.texture_paths().size() << "\n";
                }
            }
            else
            {
                loadCharTextures();
            }
        }

        showLoading(0.68f, "Uploading textures");
        if (!terrainTextures.empty())
        {
            // Log pre-normalisation format census.
            {
                std::uint32_t countBc1{}, countBc3{}, countRgba{}, countInvalid{};
                std::map<std::string, std::uint32_t> sizeDistribution;
                for (const auto& t : terrainTextures)
                {
                    if (!t.valid) { ++countInvalid; continue; }
                    auto sizeKey = std::to_string(t.width) + "x" + std::to_string(t.height);
                    sizeDistribution[sizeKey]++;
                    if (t.compressed && t.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ++countBc1;
                    else if (t.compressed && (t.vkFormat == VK_FORMAT_BC3_UNORM_BLOCK || t.vkFormat == VK_FORMAT_BC2_UNORM_BLOCK)) ++countBc3;
                    else ++countRgba;
                }
                std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                log << "Texture format census (pre-normalise): BC1=" << countBc1 << " BC3=" << countBc3
                    << " RGBA=" << countRgba << " invalid=" << countInvalid
                    << " total=" << terrainTextures.size() << "\n";
                for (const auto& [size, count] : sizeDistribution)
                    log << "  size " << size << ": " << count << "\n";
            }

            showLoading(0.70f, "Normalising textures to BC3");
            normalizeTexturesForBcUpload(terrainTextures);

            {
                std::size_t bcBytes = 0;
                for (const auto& t : terrainTextures)
                    for (const auto& mip : t.mipData) bcBytes += mip.size();
                std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                log << "Texture upload: " << terrainTextures.size() << " slots, "
                    << (bcBytes / (1024 * 1024)) << " MB BC3 mip data, uploadMode=BC3-native\n";
            }

            showLoading(0.74f, "Uploading BC3 textures");
            terrainTexturesUploaded = renderer.upload_terrain_textures(terrainTextures);
            if (terrainTexturesUploaded)
            {
                releaseDecodedTextureRam(terrainTextures);
                // Also release mip data — GPU has it now.
                for (auto& t : terrainTextures)
                    std::vector<std::vector<std::uint8_t>>().swap(t.mipData);
                std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                log << "Texture RAM: released BC3 mip data after GPU upload\n";
            }
        }

        const auto& world = runtime.state().world;
        if (!world.terrainTextureMap.empty() && world.heightMapSide > 1)
        {
            std::vector<float> tileSizes;
            tileSizes.reserve(world.terrainLayers.size());
            for (const auto& layer : world.terrainLayers)
                tileSizes.push_back(std::max(1.0f, layer.tileSize));
            renderer.upload_terrain_texture_map(
                world.terrainTextureMap,
                world.heightMapSide,
                static_cast<float>(world.mapSize),
                tileSizes.data(),
                static_cast<std::uint32_t>(tileSizes.size()));
        }

        std::vector<phoenix::renderer::TerrainVertex> terrainVertices;
        std::vector<std::uint32_t> terrainIndices;
        auto objectFuture = std::async(std::launch::async, [&runtime]() {
            return runtime.build_static_object_scene();
        });
        auto animatedObjectFuture = std::async(std::launch::async, [&runtime]() {
            return runtime.build_animated_object_scene();
        });

        runAsyncVoid([&]() { runtime.build_terrain_mesh(terrainVertices, terrainIndices); }, 0.74f, "Building terrain");
        terrainVertexCount = static_cast<std::uint32_t>(terrainVertices.size());
        terrainIndexCount = static_cast<std::uint32_t>(terrainIndices.size());
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Terrain mesh: vertices=" << terrainVertices.size()
                << " indices=" << terrainIndices.size() << "\n";
        }

        if (!renderer.set_terrain_mesh(terrainVertices, terrainIndices))
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Terrain mesh upload unavailable; using CPU preview fallback\n";
            const auto previewWidth = std::max(480u, renderer.surface_width() / 2u);
            const auto previewHeight = std::max(270u, renderer.surface_height() / 2u);
            auto preview = runtime.create_3d_preview_image(previewWidth, previewHeight);
            renderer.set_preview_image(preview.width, preview.height, preview.bgra);
        }

        showLoading(0.82f, "Building objects");
        staticObjectScene = objectFuture.get();
        animatedObjectScene = animatedObjectFuture.get();
        actorAnimatedBatchStart = animatedObjectScene.batches.size();
        if (!actorScene.animatedVertices.empty() && !actorScene.animatedIndices.empty() && !actorScene.animatedInstances.empty())
        {
            const auto vertexBase = static_cast<std::uint32_t>(animatedObjectScene.vertices.size());
            const auto indexBase = static_cast<std::uint32_t>(animatedObjectScene.indices.size());
            const auto instanceBase = static_cast<std::uint32_t>(animatedObjectScene.instances.size());
            animatedObjectScene.vertices.insert(
                animatedObjectScene.vertices.end(),
                actorScene.animatedVertices.begin(),
                actorScene.animatedVertices.end());
            animatedObjectScene.indices.reserve(animatedObjectScene.indices.size() + actorScene.animatedIndices.size());
            for (const auto index : actorScene.animatedIndices)
                animatedObjectScene.indices.push_back(vertexBase + index);
            animatedObjectScene.baseInstances.insert(
                animatedObjectScene.baseInstances.end(),
                actorScene.animatedBaseInstances.begin(),
                actorScene.animatedBaseInstances.end());
            animatedObjectScene.instances.insert(
                animatedObjectScene.instances.end(),
                actorScene.animatedInstances.begin(),
                actorScene.animatedInstances.end());
            for (auto& label : actorScene.labels)
            {
                if (label.followsAnimatedInstance && label.animatedInstanceIndex != UINT32_MAX)
                    label.animatedInstanceIndex += instanceBase;
            }
            for (auto batch : actorScene.animatedBatches)
            {
                batch.firstIndex += indexBase;
                batch.firstInstance += instanceBase;
                animatedObjectScene.batches.push_back(batch);
            }
            for (const auto& bounds : actorScene.animatedBatchBounds)
            {
                phoenix::runtime::StaticObjectScene::BatchBounds converted{};
                converted.x = bounds.x;
                converted.y = bounds.y;
                converted.z = bounds.z;
                converted.radius = bounds.radius;
                animatedObjectScene.batchBounds.push_back(converted);
            }
            actorVertexAnimationStart = animatedObjectScene.vertexAnimations.size();
            // Build a map from actorScene vertex animation index ? runtime vertex animation index.
            std::unordered_map<std::uint32_t, std::size_t> actorAnimFirstVertexToRuntimeIdx;
            for (std::size_t ai = 0; ai < actorScene.vertexAnimations.size(); ++ai)
            {
                const auto& actorAnimation = actorScene.vertexAnimations[ai];
                const auto runtimeIdx = animatedObjectScene.vertexAnimations.size();
                phoenix::runtime::AnimatedObjectScene::VertexAnimation animation{};
                animation.firstVertex = vertexBase + actorAnimation.firstVertex;
                animation.vertexCount = actorAnimation.vertexCount;
                animation.frames = actorAnimation.frames;
                animation.skinData = actorAnimation.skinData;
                animation.animations = actorAnimation.animations;
                animation.hasActorSkin = actorAnimation.hasActorSkin;
                animation.isMob = actorAnimation.isMob;
                animation.worldX = actorAnimation.worldX;
                animation.worldY = actorAnimation.worldY;
                animation.worldZ = actorAnimation.worldZ;
                animation.boundingRadius = actorAnimation.boundingRadius;
                // Stagger initial gesture timer so actors don't all idle at once.
                const auto seed = static_cast<std::uint32_t>(
                    std::abs(actorAnimation.worldX * 7.3f) + std::abs(actorAnimation.worldZ * 13.7f));
                animation.gestureTimer = 5.0f + static_cast<float>(seed % 250) / 10.0f; // 5-30s
                if (animation.isMob)
                    animation.mobMoveTimer = 4.0f + static_cast<float>((seed / 17u) % 200) / 10.0f; // 4-24s
                actorAnimFirstVertexToRuntimeIdx[actorAnimation.firstVertex] = runtimeIdx;
                animatedObjectScene.vertexAnimations.push_back(std::move(animation));
            }

            // Build per-instance mob movement state.
            // Match each animated actor batch to its vertex animation by looking up which
            // vertex range the batch's indices reference and finding the animation that owns it.
            for (const auto& batch : actorScene.animatedBatches)
            {
                if (batch.indexCount == 0)
                    continue;
                // Determine which vertex this batch uses by reading its first index.
                const auto firstIdx = actorScene.animatedIndices[batch.firstIndex];
                // Find which vertex animation contains this vertex.
                std::size_t matchedAnimIdx = std::numeric_limits<std::size_t>::max();
                for (const auto& [fv, runtimeIdx] : actorAnimFirstVertexToRuntimeIdx)
                {
                    const auto& anim = animatedObjectScene.vertexAnimations[runtimeIdx];
                    if (!anim.isMob)
                        continue;
                    // Check if firstIdx falls within this animation's vertex range (in actor-local space).
                    if (firstIdx >= fv && firstIdx < fv + anim.vertexCount)
                    {
                        matchedAnimIdx = runtimeIdx;
                        break;
                    }
                }
                if (matchedAnimIdx == std::numeric_limits<std::size_t>::max())
                    continue;

                animatedObjectScene.vertexAnimations[matchedAnimIdx].totalInstances += batch.instanceCount;
                for (std::uint32_t j = 0; j < batch.instanceCount; ++j)
                {
                    const auto globalInstIdx = instanceBase + batch.firstInstance + j;
                    if (globalInstIdx >= animatedObjectScene.baseInstances.size())
                        continue;
                    const auto& inst = animatedObjectScene.baseInstances[globalInstIdx];
                    phoenix::runtime::AnimatedObjectScene::MobInstanceState mob{};
                    mob.instanceIndex = static_cast<std::uint32_t>(globalInstIdx);
                    mob.animIndex = static_cast<std::uint32_t>(matchedAnimIdx);
                    mob.spawnX = inst.position[0];
                    mob.spawnZ = inst.position[2];
                    mob.yaw = std::atan2(inst.forward[0], inst.forward[2]);
                    // Stagger movement timers per instance.
                    const auto seed = static_cast<std::uint32_t>(
                        std::abs(inst.position[0] * 11.3f) + std::abs(inst.position[2] * 7.7f));
                    mob.moveTimer = 4.0f + static_cast<float>(seed % 200) / 10.0f; // 4-24s
                    animatedObjectScene.mobInstances.push_back(mob);
                }
            }
            split_animated_actor_batches_by_cell(animatedObjectScene, actorScene,
                actorAnimatedBatchStart, instanceBase);
        }
        actorGrid.reset();
        actorBatchStart = staticObjectScene.batches.size();
        if (!actorScene.vertices.empty() && !actorScene.indices.empty() && !actorScene.instances.empty())
        {
            const auto vertexBase = static_cast<std::uint32_t>(staticObjectScene.vertices.size());
            const auto indexBase = static_cast<std::uint32_t>(staticObjectScene.indices.size());
            staticObjectScene.vertices.insert(staticObjectScene.vertices.end(),
                actorScene.vertices.begin(), actorScene.vertices.end());
            staticObjectScene.indices.reserve(staticObjectScene.indices.size() + actorScene.indices.size());
            for (const auto index : actorScene.indices)
                staticObjectScene.indices.push_back(vertexBase + index);

            for (std::size_t b = 0; b < actorScene.batches.size(); ++b)
            {
                const auto& origBatch = actorScene.batches[b];
                const auto ti = static_cast<std::uint32_t>(actorGrid.staticTemplates.size());
                actorGrid.staticTemplates.push_back({
                    indexBase + origBatch.firstIndex, origBatch.indexCount });
                for (std::uint32_t j = 0; j < origBatch.instanceCount; ++j)
                {
                    const auto& inst = actorScene.instances[origBatch.firstInstance + j];
                    const auto cx = ActorGrid::to_cell(inst.position[0]);
                    const auto cz = ActorGrid::to_cell(inst.position[2]);
                    actorGrid.staticCells[ActorGrid::cell_key(cx, cz)].push_back({ ti, inst });
                }
            }
        }
        objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
        objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Static object mesh: vertices=" << staticObjectScene.vertices.size()
                << " indices=" << staticObjectScene.indices.size()
                << " instances=" << staticObjectScene.instances.size()
                << " batches=" << staticObjectScene.batches.size()
                << "\n";
        }
        if (!renderer.set_static_object_mesh(
            staticObjectScene.vertices,
            staticObjectScene.indices,
            staticObjectScene.instances,
            staticObjectScene.batches))
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Static object mesh upload unavailable\n";
        }

        // Upload indirect draw data for GPU frustum culling.
        if (renderer.upload_indirect_draw_data(staticObjectScene.batches, extract_gpu_bounds(staticObjectScene)))
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "GPU frustum culling: " << staticObjectScene.batches.size() << " batches uploaded to indirect draw\n";
        }

        if (!animatedObjectScene.vertices.empty()
            && !renderer.set_animated_object_mesh(
                animatedObjectScene.vertices,
                animatedObjectScene.indices,
                animatedObjectScene.instances,
                animatedObjectScene.batches))
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Animated object mesh upload unavailable\n";
        }

        // Upload GPU skinning source vertices for compute skinning.
        gpuSkinningActive = false;
        if (actorVertexAnimationStart < animatedObjectScene.vertexAnimations.size())
        {
            // Build source vertex array for all actor vertex animations.
            std::vector<phoenix::world::GpuSkinSourceVertex> allSkinSources;
            allSkinSources.resize(animatedObjectScene.vertices.size()); // sized for full vertex buffer
            for (std::size_t i = actorVertexAnimationStart; i < animatedObjectScene.vertexAnimations.size(); ++i)
            {
                const auto& anim = animatedObjectScene.vertexAnimations[i];
                if (!anim.hasActorSkin || anim.skinData.sourceVertices.empty())
                    continue;
                auto gpuSources = phoenix::world::build_gpu_skin_sources(anim.skinData);
                const auto count = std::min(gpuSources.size(), static_cast<std::size_t>(anim.vertexCount));
                for (std::size_t v = 0; v < count; ++v)
                    allSkinSources[anim.firstVertex + v] = gpuSources[v];
            }
            if (renderer.upload_skin_source_vertices(allSkinSources.data(), allSkinSources.size()))
                gpuSkinningActive = true;
        }
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "GPU compute skinning: " << (gpuSkinningActive ? "active" : "disabled (CPU fallback)") << "\n";
        }

        showLoading(0.90f, "Finalizing scene");
        uploadDebugGizmos();

        worldCollisionMesh = runAsync([&]() { return runtime.build_collision_mesh(); }, 0.92f, "Building collision");

        // Re-sample actor heights now that collision mesh is available.
        // Actors were initially placed using terrain-only height; now they
        // should also stand on walkable collision surfaces (bridges, etc.).
        {
            // Re-sample static actor instances in grid cells.
            for (auto& [cellKey_, entries] : actorGrid.staticCells)
            {
                for (auto& entry : entries)
                {
                    heightSamplerCtx.lastCharacterY = entry.inst.position[1];
                    entry.inst.position[1] = character_height_sampler(
                        entry.inst.position[0], entry.inst.position[2], &heightSamplerCtx);
                }
            }
            // Animated actor instances (NPCs) + vertex animation world positions.
            if (actorAnimatedBatchStart < animatedObjectScene.batches.size())
            {
                const auto firstActorInstance = animatedObjectScene.batches[actorAnimatedBatchStart].firstInstance;
                for (std::size_t i = firstActorInstance; i < animatedObjectScene.instances.size(); ++i)
                {
                    auto& inst = animatedObjectScene.instances[i];
                    const float x = inst.position[0];
                    const float z = inst.position[2];
                    heightSamplerCtx.lastCharacterY = inst.position[1];
                    inst.position[1] = character_height_sampler(x, z, &heightSamplerCtx);
                }
                for (std::size_t i = firstActorInstance; i < animatedObjectScene.baseInstances.size(); ++i)
                {
                    auto& inst = animatedObjectScene.baseInstances[i];
                    const float x = inst.position[0];
                    const float z = inst.position[2];
                    heightSamplerCtx.lastCharacterY = inst.position[1];
                    inst.position[1] = character_height_sampler(x, z, &heightSamplerCtx);
                }
            }
            if (actorVertexAnimationStart < animatedObjectScene.vertexAnimations.size())
            {
                for (std::size_t i = actorVertexAnimationStart; i < animatedObjectScene.vertexAnimations.size(); ++i)
                {
                    auto& anim = animatedObjectScene.vertexAnimations[i];
                    heightSamplerCtx.lastCharacterY = anim.worldY;
                    anim.worldY = character_height_sampler(anim.worldX, anim.worldZ, &heightSamplerCtx);
                }
            }
        }

        // Finalize actor grid and perform initial nearby-instance upload.
        actorGrid.worldInstances = staticObjectScene.instances;
        actorGrid.worldBatches = staticObjectScene.batches;
        actorGrid.worldBounds = staticObjectScene.batchBounds;
        actorGrid.built = true;
        {
            float initCamX, initCamY, initCamZ, initYaw, initPitch;
            runtime.camera_state(initCamX, initCamY, initCamZ, initYaw, initPitch);
            actorGrid.rebuild(initCamX, initCamZ, actorViewDistance, staticObjectScene, actorBatchStart);
            actorGrid.lastCX = ActorGrid::to_cell(initCamX);
            actorGrid.lastCZ = ActorGrid::to_cell(initCamZ);
            renderer.update_static_object_instances(staticObjectScene.instances, staticObjectScene.batches);
            renderer.update_indirect_draw_data(staticObjectScene.batches, extract_gpu_bounds(staticObjectScene));
            objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
            objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
        }
        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "Actor grid: " << actorGrid.staticCells.size() << " cells, "
                << actorGrid.staticTemplates.size() << " static templates, "
                << (animatedObjectScene.batches.size() - actorAnimatedBatchStart)
                << " animated sub-batches\n";
        }

        // Upload character mesh (initial bind pose).
        if (characterLoaded && characterSystem.ready())
            uploadCharacterMesh();

        forceVisibilityUpdate = true;
        showLoading(1.0f, "Ready");
    };

    uploadCurrentWorld();
    applyFogSettings();
    window.set_title(runtime.window_title(renderer.adapter_name(), displayedFps, fogEnabled));

    using clock = std::chrono::steady_clock;
    auto lastFrame = clock::now();
    auto lastTitleUpdate = lastFrame;
    float totalTime = 0.0f;
    std::uint32_t framesSinceTitleUpdate = 0;
    bool fogToggleWasDown = false;
    bool playToggleWasDown = false;
    auto lastClientSize = window.client_size();

    while (window.pump_messages())
    {
        const auto currentClientSize = window.client_size();
        const bool minimized = currentClientSize.first <= 0 || currentClientSize.second <= 0;
        if (!minimized && currentClientSize != lastClientSize)
        {
            renderer.resize(
                static_cast<std::uint32_t>(currentClientSize.first),
                static_cast<std::uint32_t>(currentClientSize.second));
            lastClientSize = currentClientSize;
        }
        if (minimized)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
            continue;
        }

        const auto now = clock::now();
        const auto deltaSeconds = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        ++framesSinceTitleUpdate;
        totalTime += deltaSeconds;
        perfHud.push_frametime(deltaSeconds);
        renderer.update_water_time(totalTime);
        if (!animatedObjectScene.vertices.empty())
        {
            float camX, camY, camZ, camYaw, camPitch;
            if (playableMode && characterSystem.ready())
                characterSystem.camera_state(camX, camY, camZ, camYaw, camPitch);
            else
                runtime.camera_state(camX, camY, camZ, camYaw, camPitch);

            if (gpuSkinningActive && renderer.gpu_skinning_ready())
            {
                // GPU skinning path: CPU handles animation state + mob movement + VANI only.
                // Actor vertex skinning is done by compute shader.
                constexpr float kAnimationRange = 180.0f;

                // Run animation update (mob movement, VANI, gesture timing) but skip CPU skinning.
                runtime.update_animated_object_scene(animatedObjectScene, totalTime, deltaSeconds, camX, camY, camZ, actorVertexAnimationStart, true);

                // Compute bone matrices for visible actors and queue GPU dispatches.
                std::vector<float> allBoneMatrices;
                for (std::size_t i = actorVertexAnimationStart; i < animatedObjectScene.vertexAnimations.size(); ++i)
                {
                    const auto& anim = animatedObjectScene.vertexAnimations[i];
                    if (!anim.hasActorSkin || anim.skinData.sourceVertices.empty())
                        continue;

                    // Distance cull check (same as CPU path).
                    const float dx = anim.worldX - camX;
                    const float dy = anim.worldY - camY;
                    const float dz = anim.worldZ - camZ;
                    const float distSq = dx * dx + dy * dy + dz * dz;
                    const float effectiveRange = kAnimationRange + anim.boundingRadius;
                    if (distSq > effectiveRange * effectiveRange)
                        continue;

                    // Determine active animation and frame (mirrors CPU logic).
                    const auto& breathAnim = anim.animations.breath;
                    const auto& idleAnim = anim.animations.idle;
                    const auto& walkAnim = anim.animations.walk;
                    const auto& runAnim = anim.animations.run;

                    auto animFrameCount = [](const phoenix::world::CharacterAnimation& a) -> float {
                        if (!a.parsed || a.endKeyframe <= a.startKeyframe)
                            return 1.0f;
                        return static_cast<float>(a.endKeyframe - a.startKeyframe + 1);
                    };

                    const phoenix::world::CharacterAnimation* activeAnim = &breathAnim;
                    float frameF = 0.0f;

                    if (anim.isMob)
                    {
                        const auto total = std::max(1u, anim.totalInstances);
                        const float movingRatio = static_cast<float>(anim.movingCount) / static_cast<float>(total);
                        const float runningRatio = static_cast<float>(anim.runningCount) / static_cast<float>(total);
                        float playbackRate = 12.0f;
                        if (runningRatio > 0.4f && runAnim.parsed)
                        {
                            activeAnim = &runAnim;
                            playbackRate = 18.0f;
                        }
                        else if (movingRatio > 0.4f && walkAnim.parsed)
                        {
                            activeAnim = &walkAnim;
                            playbackRate = 14.0f;
                        }
                        else if (anim.playingGesture && idleAnim.parsed)
                        {
                            activeAnim = &idleAnim;
                            frameF = (anim.gestureTimer / 5.0f) * animFrameCount(idleAnim);
                        }
                        if (activeAnim != &idleAnim || !anim.playingGesture)
                            frameF = std::fmod(totalTime * playbackRate, animFrameCount(*activeAnim));
                    }
                    else
                    {
                        // NPC
                        float playbackRate = 12.0f;
                        if (anim.playingGesture && idleAnim.parsed)
                        {
                            activeAnim = &idleAnim;
                            frameF = (anim.gestureTimer / 5.0f) * animFrameCount(idleAnim);
                        }
                        else
                        {
                            frameF = std::fmod(totalTime * playbackRate, animFrameCount(*activeAnim));
                        }
                    }

                    if (!activeAnim->parsed)
                        continue;

                    const auto boneOffset = static_cast<std::uint32_t>(allBoneMatrices.size() / 16);
                    const auto boneCount = phoenix::world::compute_skin_matrices(
                        anim.skinData, *activeAnim, frameF, allBoneMatrices);
                    if (boneCount == 0)
                        continue;

                    renderer.dispatch_skin_compute(anim.firstVertex, anim.vertexCount, boneOffset);
                }

                // Upload all bone matrices for this frame.
                if (!allBoneMatrices.empty())
                {
                    renderer.upload_skin_matrices(
                        allBoneMatrices.data(),
                        static_cast<std::uint32_t>(allBoneMatrices.size() / 16));
                }
            }
            else
            {
                // CPU skinning fallback path (unchanged).
                runtime.update_animated_object_scene(animatedObjectScene, totalTime, deltaSeconds, camX, camY, camZ, actorVertexAnimationStart);
            }

            renderer.update_animated_object_scene(animatedObjectScene.vertices, animatedObjectScene.instances);
        }

        const auto fogToggleDown = window.is_key_down(SDLK_f);
        if (fogToggleDown && !fogToggleWasDown)
        {
            fogEnabled = !fogEnabled;
            applyFogSettings();
        }
        fogToggleWasDown = fogToggleDown;

        // Toggle playable mode with P key.
        const auto playToggleDown = window.is_key_down(SDLK_p);
        if (playToggleDown && !playToggleWasDown && characterLoaded)
        {
            playableMode = !playableMode;
            if (playableMode && runtime.state().world.isDungeon && characterSystem.ready())
            {
                float freeCamX{};
                float freeCamY{};
                float freeCamZ{};
                float freeCamYaw{};
                float freeCamPitch{};
                runtime.camera_state(freeCamX, freeCamY, freeCamZ, freeCamYaw, freeCamPitch);
                const auto spawn = find_dungeon_playable_spawn(worldCollisionMesh, freeCamX, freeCamY, freeCamZ);
                if (spawn.valid)
                {
                    characterSystem.set_world_position(spawn.x, spawn.y, spawn.z, freeCamYaw);
                    heightSamplerCtx.lastCharacterY = spawn.y;
                    uploadCharacterMesh();
                    std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                    log << "Playable dungeon spawn: x=" << spawn.x
                        << " y=" << spawn.y
                        << " z=" << spawn.z
                        << " camera=(" << freeCamX << "," << freeCamY << "," << freeCamZ << ")\n";
                }
                else
                {
                    std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
                    log << "Playable dungeon spawn unavailable: collisionTriangles="
                        << worldCollisionMesh.triangles.size() << "\n";
                }
            }
            renderer.set_character_visible(playableMode);
            forceVisibilityUpdate = true;
        }
        playToggleWasDown = playToggleDown;

        if (imguiAvailable)
            renderer.begin_imgui_frame();
        const auto imguiWantsKeyboard = imguiAvailable && ImGui::GetIO().WantCaptureKeyboard;
        const auto imguiWantsMouse = imguiAvailable && ImGui::GetIO().WantCaptureMouse;

        const auto [mouseDx, mouseDy] = window.consume_mouse_delta();
        const auto mouseWheel = window.consume_mouse_wheel_delta();

        float cameraX{};
        float cameraY{};
        float cameraZ{};
        float cameraYaw{};
        float cameraPitch{};

        if (playableMode && characterLoaded && characterSystem.ready())
        {
            // ---- Playable mode: third-person character control ----
            phoenix::character::PlayableInput pInput{};
            if (!imguiWantsKeyboard)
            {
                pInput.forward = window.is_key_down(SDLK_w);
                pInput.backward = window.is_key_down(SDLK_s);
                pInput.left = window.is_key_down(SDLK_a);
                pInput.right = window.is_key_down(SDLK_d);
                pInput.jump = window.is_key_down(SDLK_SPACE);
                pInput.fast = window.is_key_down(SDLK_LSHIFT);
                pInput.yawLeft = window.is_key_down(SDLK_LEFT);
                pInput.yawRight = window.is_key_down(SDLK_RIGHT);
                pInput.pitchUp = window.is_key_down(SDLK_UP);
                pInput.pitchDown = window.is_key_down(SDLK_DOWN);
            }
            pInput.cameraDrag = !imguiWantsMouse && window.is_mouse_button_down(1);
            pInput.mouseDx = !imguiWantsMouse ? static_cast<float>(mouseDx) : 0.0f;
            pInput.mouseDy = !imguiWantsMouse ? static_cast<float>(mouseDy) : 0.0f;
            pInput.mouseWheel = !imguiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

            heightSamplerCtx.lastCharacterY = characterSystem.world_y();
            characterSystem.update(deltaSeconds, pInput);
            characterSystem.camera_state(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);

            // Upload animated vertices.
            const auto& charVerts = characterSystem.world_vertices();
            const auto* tv = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(charVerts.data());
            std::vector<phoenix::renderer::TerrainVertex> charFrame(tv, tv + charVerts.size());
            renderer.update_character_vertices(charFrame);
        }
        else
        {
            // ---- Viewer mode: free camera ----
            phoenix::runtime::CameraInput cameraInput{};
            if (!imguiWantsKeyboard)
            {
                cameraInput.forward = window.is_key_down(SDLK_w);
                cameraInput.backward = window.is_key_down(SDLK_s);
                cameraInput.left = window.is_key_down(SDLK_a);
                cameraInput.right = window.is_key_down(SDLK_d);
                cameraInput.up = window.is_key_down(SDLK_e) || window.is_key_down(SDLK_SPACE);
                cameraInput.down = window.is_key_down(SDLK_q) || window.is_key_down(SDLK_LCTRL);
                cameraInput.fast = window.is_key_down(SDLK_LSHIFT);
                cameraInput.yawLeft = window.is_key_down(SDLK_LEFT);
                cameraInput.yawRight = window.is_key_down(SDLK_RIGHT);
                cameraInput.pitchUp = window.is_key_down(SDLK_UP);
                cameraInput.pitchDown = window.is_key_down(SDLK_DOWN);
            }
            cameraInput.look = !imguiWantsMouse && window.is_mouse_button_down(1);
            cameraInput.mouseDx = !imguiWantsMouse ? static_cast<float>(mouseDx) : 0.0f;
            cameraInput.mouseDy = !imguiWantsMouse ? static_cast<float>(mouseDy) : 0.0f;
            cameraInput.wheel = !imguiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

            const auto cameraChanged = cameraInput.forward || cameraInput.backward
                || cameraInput.left || cameraInput.right
                || cameraInput.up || cameraInput.down
                || cameraInput.yawLeft || cameraInput.yawRight
                || cameraInput.pitchUp || cameraInput.pitchDown
                || (cameraInput.look && (mouseDx != 0 || mouseDy != 0))
                || cameraInput.wheel != 0.0f;
            if (cameraChanged)
                runtime.update_camera(deltaSeconds, cameraInput);

            runtime.camera_state(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);
            renderer.set_character_visible(false);
        }

        renderer.set_camera(
            cameraX,
            cameraY,
            cameraZ,
            cameraYaw,
            cameraPitch,
            static_cast<float>(std::max(1u, renderer.surface_width())) / static_cast<float>(std::max(1u, renderer.surface_height())),
            viewDistance);

        CameraView currentView{};
        currentView.x = cameraX;
        currentView.y = cameraY;
        currentView.z = cameraZ;
        currentView.yaw = cameraYaw;
        currentView.pitch = cameraPitch;
        currentView.aspect = static_cast<float>(std::max(1u, renderer.surface_width()))
            / static_cast<float>(std::max(1u, renderer.surface_height()));
        currentView.distance = viewDistance;

        if (actorGrid.built && actorsEnabled && actorGrid.camera_cell_changed(cameraX, cameraZ))
        {
            actorGrid.rebuild(cameraX, cameraZ, actorViewDistance, staticObjectScene, actorBatchStart);
            renderer.update_static_object_instances(staticObjectScene.instances, staticObjectScene.batches);
            renderer.update_indirect_draw_data(staticObjectScene.batches, extract_gpu_bounds(staticObjectScene));
            objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
            objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
            forceVisibilityUpdate = true;
        }

        const auto cullMoveDx = currentView.x - lastCullView.x;
        const auto cullMoveDy = currentView.y - lastCullView.y;
        const auto cullMoveDz = currentView.z - lastCullView.z;
        const auto cullMoveSq = cullMoveDx * cullMoveDx + cullMoveDy * cullMoveDy + cullMoveDz * cullMoveDz;
        const bool visibilityNeedsUpdate = forceVisibilityUpdate
            || cullMoveSq > 96.0f * 96.0f
            || std::abs(currentView.yaw - lastCullView.yaw) > 0.08f
            || std::abs(currentView.pitch - lastCullView.pitch) > 0.06f
            || std::abs(currentView.distance - lastCullView.distance) > 32.0f
            || std::abs(currentView.aspect - lastCullView.aspect) > 0.01f;
        if (visibilityNeedsUpdate)
        {
            const auto visibleTerrainRanges = build_visible_terrain_ranges(runtime, currentView);
            renderer.set_terrain_draw_ranges(visibleTerrainRanges);
            std::uint32_t visibleTerrainIndexCount{};
            for (const auto& range : visibleTerrainRanges)
                visibleTerrainIndexCount += range.indexCount;
            if (visibleTerrainIndexCount > 0)
                terrainIndexCount = visibleTerrainIndexCount;

            const float effectiveActorDistance = actorsEnabled ? actorViewDistance : -1.0f;
            if (renderer.indirect_draw_ready())
            {
                // GPU frustum culling handles static objects — no CPU culling needed.
                // The compute shader runs each frame with the current camera state.
                objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
            }
            else
            {
                // Fallback: CPU frustum culling.
                std::vector<phoenix::renderer::ObjectBatch> visibleBatches;
                build_visible_object_batches(staticObjectScene, currentView, actorBatchStart, effectiveActorDistance, visibleBatches);
                renderer.set_static_object_batches(visibleBatches);
                std::uint32_t visibleObjectInstances{};
                for (const auto& batch : visibleBatches)
                    visibleObjectInstances += batch.instanceCount;
                objectInstanceCount = visibleObjectInstances;
                objectBatchCount = static_cast<std::uint32_t>(visibleBatches.size());
            }

            if (!animatedObjectScene.batches.empty())
            {
                std::vector<phoenix::renderer::ObjectBatch> visibleAnimatedBatches;
                build_visible_animated_batches(animatedObjectScene, currentView, actorAnimatedBatchStart, effectiveActorDistance, visibleAnimatedBatches);
                renderer.set_animated_object_batches(visibleAnimatedBatches);
            }

            lastCullView = currentView;
            forceVisibilityUpdate = false;
        }

        if (imguiAvailable)
        {
            const bool prevActorsEnabled = actorsEnabled;
            const auto panelResult = draw_editor_panel(
                runtime,
                renderer,
                fogEnabled,
                showSoundGizmos,
                showMusicGizmos,
                showPortalGizmos,
                showEffectGizmos,
                showNamePlates,
                showCollisionDebug,
                playMapSounds,
                playMapMusic,
                selectedMapIndex,
                viewDistance,
                actorViewDistance,
                actorsEnabled,
                weatherMode,
                characterOptions,
                selectedCharacterOption,
                characterAppearance,
                characterSystem,
                weaponEffect,
                cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);

            if (panelResult.loadRequested)
                pendingMapLoad = static_cast<std::size_t>(std::max(0, selectedMapIndex));
            else if (panelResult.viewDistanceChanged)
            {
                applyFogSettings();
                actorGrid.lastCX = INT_MAX;
                forceVisibilityUpdate = true;
            }
            else if (panelResult.weatherChanged)
            {
                applyFogSettings();
            }
            else if (panelResult.debugGizmosChanged)
            {
                uploadDebugGizmos();
            }

            if (panelResult.characterChanged)
                reloadCharacterIntoRenderer();

            if (actorsEnabled != prevActorsEnabled)
            {
                actorGrid.lastCX = INT_MAX;
                forceVisibilityUpdate = true;
            }

            if (actorsEnabled && showNamePlates && !actorScene.labels.empty())
            {
                struct VisibleLabel
                {
                    const phoenix::world::ActorScene::Label* label{};
                    float distanceSq{};
                    ImVec2 screen{};
                };

                std::vector<VisibleLabel> labels;
                labels.reserve(std::min<std::size_t>(actorScene.labels.size(), 128));
                const auto width = static_cast<float>(renderer.surface_width());
                const auto height = static_cast<float>(renderer.surface_height());
                const float namePlateOriginX = (playableMode && characterLoaded && characterSystem.ready())
                    ? characterSystem.world_x()
                    : currentView.x;
                const float namePlateOriginY = (playableMode && characterLoaded && characterSystem.ready())
                    ? characterSystem.world_y()
                    : currentView.y;
                const float namePlateOriginZ = (playableMode && characterLoaded && characterSystem.ready())
                    ? characterSystem.world_z()
                    : currentView.z;
                const float maxNamePlateDistanceSq = kNamePlateVisibleDistance * kNamePlateVisibleDistance;
                for (const auto& label : actorScene.labels)
                {
                    float labelX = label.x;
                    float labelY = label.y;
                    float labelZ = label.z;
                    if (label.followsAnimatedInstance
                        && label.animatedInstanceIndex < animatedObjectScene.instances.size())
                    {
                        const auto& actorInstance = animatedObjectScene.instances[label.animatedInstanceIndex];
                        labelX = actorInstance.position[0];
                        labelY = actorInstance.position[1] + label.offsetY;
                        labelZ = actorInstance.position[2];
                    }

                    const auto originDx = labelX - namePlateOriginX;
                    const auto originDy = labelY - namePlateOriginY;
                    const auto originDz = labelZ - namePlateOriginZ;
                    const auto namePlateDistanceSq = originDx * originDx + originDy * originDy + originDz * originDz;
                    if (namePlateDistanceSq > maxNamePlateDistanceSq)
                        continue;
                    if (!sphere_visible(currentView, labelX, labelY, labelZ, label.radius))
                        continue;
                    ImVec2 screen{};
                    if (!project_world_to_screen(currentView, labelX, labelY, labelZ, width, height, screen))
                        continue;
                    labels.push_back({ &label, namePlateDistanceSq, screen });
                }

                std::ranges::sort(labels, [](const auto& lhs, const auto& rhs) {
                    return lhs.distanceSq < rhs.distanceSq;
                });

                auto* drawList = ImGui::GetForegroundDrawList();
                const auto maxLabels = std::min<std::size_t>(labels.size(), 80);
                for (std::size_t i = 0; i < maxLabels; ++i)
                {
                    const auto& item = labels[i];
                    const auto textSize = ImGui::CalcTextSize(item.label->text.c_str());
                    const ImVec2 pos{ item.screen.x - textSize.x * 0.5f, item.screen.y - textSize.y * 0.5f };
                    drawList->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 190), item.label->text.c_str());
                    drawList->AddText(pos, IM_COL32(255, 238, 180, 235), item.label->text.c_str());
                }
            }

            draw_weather_overlay(
                weatherMode,
                currentView,
                totalTime,
                static_cast<float>(renderer.surface_width()),
                static_cast<float>(renderer.surface_height()));

            // Performance HUD overlay.
            perfHud.visibleBatches = objectBatchCount;
            perfHud.visibleInstances = objectInstanceCount;
            perfHud.totalTriangles = terrainVertexCount / 3u;
            perfHud.actorCount = actorScene.npcCount + actorScene.monsterCount;
            std::uint32_t mobsMovingNow = 0;
            for (const auto& mob : animatedObjectScene.mobInstances)
                if (mob.moving) ++mobsMovingNow;
            perfHud.mobsMoving = mobsMovingNow;
            draw_perf_hud(perfHud, static_cast<float>(renderer.surface_width()));
        }

        if (audioAvailable)
        {
            float listenerX = cameraX;
            float listenerY = cameraY;
            float listenerZ = cameraZ;
            if (playableMode && characterLoaded && characterSystem.ready())
            {
                listenerX = characterSystem.world_x();
                listenerY = characterSystem.world_y();
                listenerZ = characterSystem.world_z();
            }

            audioSystem.update(deltaSeconds, build_audible_tracks(
                mapAudioScene,
                listenerX,
                listenerY,
                listenerZ,
                playMapMusic,
                playMapSounds));
        }

        // Procedural weapon-effect particles (anchored to the equipped weapon's
        // attach bone, recomputed in characterSystem.update()).
        weaponEffect.update(deltaSeconds, characterSystem.weapon_attachment(), renderer);

        renderer.render_frame();

        if (pendingMapLoad)
        {
            const auto mapIdx = *pendingMapLoad;
            pendingMapLoad.reset();
            renderer.enter_loading_mode();
            showLoading(0.05f, "Changing map");
            if (runAsync([&]() { return runtime.load_world_map(mapIdx); }, 0.10f, "Loading world"))
            {
                uploadCurrentWorld();
                applyFogSettings();
            }
            lastFrame = clock::now();
        }

        if (now - lastTitleUpdate > std::chrono::seconds(1))
        {
            const auto titleSeconds = std::chrono::duration<float>(now - lastTitleUpdate).count();
            displayedFps = titleSeconds > 0.0f ? static_cast<float>(framesSinceTitleUpdate) / titleSeconds : 0.0f;
            framesSinceTitleUpdate = 0;
            window.set_title(runtime.window_title(renderer.adapter_name(), displayedFps, fogEnabled));
            lastTitleUpdate = now;
        }
    }

    renderer.shutdown();
    audioSystem.shutdown();
    return 0;
}
