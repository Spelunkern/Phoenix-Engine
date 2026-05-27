#include "audio/audio_system.h"
#include "character/character_system.h"
#include "runtime/phoenix_runtime.h"
#include "platform/win32_window.h"
#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"
#include "world/actor_scene.h"
#include "generated/loading_icon_bgra.inc"

#include "imgui.h"


#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
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

#include <psapi.h>
#include <winternl.h>
#pragma comment(lib, "ntdll.lib")

namespace
{
    constexpr const wchar_t* kAppTitle = L"Phoenix Engine";
    constexpr const char* kLogName = "PhoenixEngine.log";
    constexpr std::size_t kWaterTextureLayer = 62;
    constexpr std::size_t kSkyTextureLayer = 63;
    constexpr std::size_t kPrimaryCloudTextureLayer = 64;
    constexpr std::size_t kSecondaryCloudTextureLayer = 65;
    constexpr std::size_t kAssetTextureLayerBase = 66;
    constexpr std::size_t kMaxLoggedAssetTextures = 80;
    // Atmospheric rendering: fogEnd must be well inside viewDistance so
    // exponential fog fully covers geometry before the cull boundary.
    // This eliminates pop-in without needing alpha blending on opaques.
    constexpr float kFogStartRatio = 0.38f;   // fog begins at 38% of viewDistance
    constexpr float kFogEndRatio = 0.82f;     // fog is ~100% opaque at 82% of viewDistance
    constexpr float kTanHalfFov = 0.7002f;
    constexpr float kSoundAudibleRadiusScale = 1.6f;
    constexpr float kSoundAudibleRadiusBonus = 16.0f;
    constexpr float kNamePlateVisibleDistance = 20.0f;
    constexpr float kWeatherWaterY = 0.0f;

    enum class WeatherMode
    {
        Default,
        Storm,
        Snowstorm,
        Sunset,
        Night,
    };

    // Height sampler context — terrain + collision mesh floor surfaces.
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

    // Collision callback — triangle mesh collision against world objects.
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
        std::wstring path(MAX_PATH, L'\0');
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return std::filesystem::current_path();

        path.resize(length);
        return std::filesystem::path(path).parent_path();
    }

    void apply_renderer_fog(
        phoenix::renderer::VulkanRenderer& renderer,
        const phoenix::runtime::PhoenixRuntime& runtime,
        bool fogEnabled,
        float viewDistance,
        WeatherMode weatherMode)
    {
        const auto& world = runtime.state().world;
        std::array<float, 3> weatherFog{
            world.fogColor[0],
            world.fogColor[1],
            world.fogColor[2],
        };
        std::array<float, 12> skyTuning{
            0.62f, 0.0f, 1.0f, 0.0f,
            1.35f, 0.78f, 0.34f, 0.12f,
            0.82f, 1.10f, 0.22f, 0.06f,
        };
        if (weatherMode == WeatherMode::Storm)
        {
            weatherFog = { 0.20f, 0.22f, 0.25f };
            skyTuning[3] = 1.0f;
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            weatherFog = { 0.55f, 0.57f, 0.60f };
            skyTuning[3] = 2.0f;
        }
        else if (weatherMode == WeatherMode::Sunset)
        {
            weatherFog = { 0.78f, 0.42f, 0.24f };
            skyTuning[3] = 3.0f;
        }
        else if (weatherMode == WeatherMode::Night)
        {
            weatherFog = { 0.035f, 0.045f, 0.075f };
            skyTuning[3] = 4.0f;
        }
        renderer.set_sky_tuning(skyTuning.data(), static_cast<std::uint32_t>(skyTuning.size()));

        if (!fogEnabled)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                100000.0f,
                100001.0f,
                world.parsedSky && !world.skyFileName.empty());
            return;
        }

        // Atmospheric fog: starts gradually, reaches full opacity well before cull edge.
        auto fogStart = std::max(80.0f, viewDistance * kFogStartRatio);
        auto fogEnd = std::max(fogStart + 100.0f, viewDistance * kFogEndRatio);
        if (weatherMode == WeatherMode::Storm)
        {
            fogStart = std::max(40.0f, viewDistance * 0.22f);
            fogEnd = std::max(fogStart + 90.0f, viewDistance * 0.64f);
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            fogStart = std::max(28.0f, viewDistance * 0.16f);
            fogEnd = std::max(fogStart + 75.0f, viewDistance * 0.52f);
        }
        renderer.set_sky_settings(
            weatherFog.data(),
            fogStart,
            fogEnd,
            world.parsedSky && !world.skyFileName.empty());
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

    struct CharacterOption
    {
        std::string raceFolder;
        std::string prefix;
        std::string label;
        std::vector<int> armorIndices;
        std::vector<int> faceIndices;
        std::vector<int> hairIndices;
    };


    std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::optional<std::pair<std::string, int>> parse_character_part(std::string stem, std::string_view part)
    {
        stem = lower_ascii(std::move(stem));
        const auto marker = "_" + std::string(part);
        const auto markerPos = stem.find(marker);
        if (markerPos == std::string::npos || markerPos < 4)
            return std::nullopt;
        const auto indexPos = markerPos + marker.size();
        if (indexPos + 3 > stem.size()
            || !std::isdigit(static_cast<unsigned char>(stem[indexPos]))
            || !std::isdigit(static_cast<unsigned char>(stem[indexPos + 1]))
            || !std::isdigit(static_cast<unsigned char>(stem[indexPos + 2])))
        {
            return std::nullopt;
        }
        return std::make_pair(stem.substr(0, markerPos), std::stoi(stem.substr(indexPos, 3)));
    }

    std::vector<CharacterOption> scan_character_options(const std::filesystem::path& dataRoot)
    {
        struct Temp
        {
            std::string raceFolder;
            std::string prefix;
            std::set<int> torso;
            std::set<int> lower;
            std::set<int> hand;
            std::set<int> boots;
            std::set<int> face;
            std::set<int> hair;
        };

        std::map<std::string, Temp> byKey;
        const auto characterRoot = dataRoot / "Character";
        if (!std::filesystem::exists(characterRoot))
            return {};

        for (const auto& raceEntry : std::filesystem::directory_iterator(characterRoot))
        {
            if (!raceEntry.is_directory())
                continue;
            const auto meshRoot = raceEntry.path() / "3DC";
            if (!std::filesystem::exists(meshRoot))
                continue;
            const auto raceFolder = raceEntry.path().filename().string();
            for (const auto& entry : std::filesystem::directory_iterator(meshRoot))
            {
                if (!entry.is_regular_file() || lower_ascii(entry.path().extension().string()) != ".3dc")
                    continue;

                const auto stem = entry.path().stem().string();
                const std::pair<std::string_view, std::set<int> Temp::*> parts[] = {
                    { "torso", &Temp::torso },
                    { "lower", &Temp::lower },
                    { "hand", &Temp::hand },
                    { "boots", &Temp::boots },
                    { "face", &Temp::face },
                    { "hair", &Temp::hair },
                };
                for (const auto& [partName, member] : parts)
                {
                    if (auto parsed = parse_character_part(stem, partName))
                    {
                        const auto key = raceFolder + "|" + parsed->first;
                        auto& temp = byKey[key];
                        temp.raceFolder = raceFolder;
                        temp.prefix = parsed->first;
                        (temp.*member).insert(parsed->second);
                    }
                }
            }
        }

        std::vector<CharacterOption> options;
        options.reserve(byKey.size());
        for (auto& [_, temp] : byKey)
        {
            std::vector<int> armor;
            std::set_intersection(
                temp.torso.begin(), temp.torso.end(),
                temp.lower.begin(), temp.lower.end(),
                std::back_inserter(armor));
            std::vector<int> armor2;
            std::set_intersection(
                armor.begin(), armor.end(),
                temp.hand.begin(), temp.hand.end(),
                std::back_inserter(armor2));
            armor.clear();
            std::set_intersection(
                armor2.begin(), armor2.end(),
                temp.boots.begin(), temp.boots.end(),
                std::back_inserter(armor));

            if (armor.empty() || temp.face.empty() || temp.hair.empty())
                continue;

            CharacterOption option{};
            option.raceFolder = temp.raceFolder;
            option.prefix = temp.prefix;
            option.label = temp.raceFolder + " / " + temp.prefix;
            option.armorIndices = std::move(armor);
            option.faceIndices.assign(temp.face.begin(), temp.face.end());
            option.hairIndices.assign(temp.hair.begin(), temp.hair.end());
            options.push_back(std::move(option));
        }

        std::ranges::sort(options, [](const auto& lhs, const auto& rhs) {
            return lhs.label < rhs.label;
        });
        return options;
    }

    int nearest_available(int value, const std::vector<int>& values)
    {
        if (values.empty())
            return value;
        auto best = values.front();
        auto bestDistance = std::abs(best - value);
        for (const auto candidate : values)
        {
            const auto distance = std::abs(candidate - value);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
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

        batches.reserve(scene.batches.size());
        for (std::size_t i = 0; i < scene.batches.size(); ++i)
        {
            const auto& bounds = scene.batchBounds[i];
            auto batchView = view;
            if (i >= actorBatchStart)
                batchView.distance = std::min(batchView.distance, actorViewDistance);
            if (sphere_visible(batchView, bounds.x, bounds.y, bounds.z, bounds.radius))
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
            const auto& bounds = scene.batchBounds[i];
            auto batchView = view;
            if (i >= actorAnimatedBatchStart)
                batchView.distance = std::min(batchView.distance, actorViewDistance);
            if (sphere_visible(batchView, bounds.x, bounds.y, bounds.z, bounds.radius))
                batches.push_back(scene.batches[i]);
        }
    }

    // ====================================================================
    // Performance HUD (MangoHud-style overlay)
    // ====================================================================
    struct PerfHudState
    {
        static constexpr std::size_t kHistorySize = 128;
        static constexpr std::uint32_t kMaxCores = 64;
        float frametimeHistory[kHistorySize]{};
        std::size_t historyIndex{};
        float fpsSmoothed{};
        float frametimeMs{};
        float frametimeMin{ 999.0f };
        float frametimeMax{};
        float frametimeAvg{};
        std::uint32_t visibleBatches{};
        std::uint32_t visibleInstances{};
        std::uint32_t totalTriangles{};
        std::uint32_t actorCount{};
        std::uint32_t mobsMoving{};
        bool visible{ true };

        // System metrics (updated periodically).
        float ramUsedMB{};
        float ramTotalMB{};
        float ramPercent{};
        float processRamMB{};
        float cpuPercent{};
        std::uint32_t cpuCores{};
        float coreUsage[kMaxCores]{};
        std::string gpuName;
        float vramUsedMB{};
        float vramTotalMB{};
        float systemUpdateTimer{};
        phoenix::renderer::VulkanRenderer* renderer{};

        // Per-core CPU state.
        struct CoreTimes { ULONGLONG idle{}; ULONGLONG kernel{}; ULONGLONG user{}; };
        CoreTimes lastCoreTimes[kMaxCores]{};
        bool cpuInitialized{};

        void initialize_system_info()
        {
            SYSTEM_INFO sysInfo{};
            GetSystemInfo(&sysInfo);
            cpuCores = std::min(static_cast<std::uint32_t>(sysInfo.dwNumberOfProcessors), kMaxCores);
        }

        void update_system_metrics()
        {
            // RAM.
            MEMORYSTATUSEX memStatus{};
            memStatus.dwLength = sizeof(memStatus);
            if (GlobalMemoryStatusEx(&memStatus))
            {
                ramTotalMB = static_cast<float>(memStatus.ullTotalPhys) / (1024.0f * 1024.0f);
                const auto usedBytes = memStatus.ullTotalPhys - memStatus.ullAvailPhys;
                ramUsedMB = static_cast<float>(usedBytes) / (1024.0f * 1024.0f);
                ramPercent = static_cast<float>(memStatus.dwMemoryLoad);
            }

            // Process RAM.
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
                processRamMB = static_cast<float>(pmc.WorkingSetSize) / (1024.0f * 1024.0f);

            // Per-core CPU usage via NtQuerySystemInformation.
            struct SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION {
                LARGE_INTEGER IdleTime;
                LARGE_INTEGER KernelTime;
                LARGE_INTEGER UserTime;
                LARGE_INTEGER DpcTime;
                LARGE_INTEGER InterruptTime;
                ULONG InterruptCount;
            };
            std::vector<SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION> cpuInfo(cpuCores);
            ULONG returnLength = 0;
            const auto status = NtQuerySystemInformation(
                static_cast<SYSTEM_INFORMATION_CLASS>(8), // SystemProcessorPerformanceInformation
                cpuInfo.data(),
                static_cast<ULONG>(cpuInfo.size() * sizeof(SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION)),
                &returnLength);
            if (status == 0) // STATUS_SUCCESS
            {
                float totalUsage = 0.0f;
                for (std::uint32_t i = 0; i < cpuCores; ++i)
                {
                    const auto idle = static_cast<ULONGLONG>(cpuInfo[i].IdleTime.QuadPart);
                    const auto kernel = static_cast<ULONGLONG>(cpuInfo[i].KernelTime.QuadPart);
                    const auto user = static_cast<ULONGLONG>(cpuInfo[i].UserTime.QuadPart);
                    if (cpuInitialized)
                    {
                        const auto idleDiff = idle - lastCoreTimes[i].idle;
                        const auto totalDiff = (kernel - lastCoreTimes[i].kernel) + (user - lastCoreTimes[i].user);
                        if (totalDiff > 0)
                            coreUsage[i] = (1.0f - static_cast<float>(idleDiff) / static_cast<float>(totalDiff)) * 100.0f;
                        else
                            coreUsage[i] = 0.0f;
                    }
                    lastCoreTimes[i] = { idle, kernel, user };
                    totalUsage += coreUsage[i];
                }
                cpuPercent = totalUsage / static_cast<float>(cpuCores);
                cpuInitialized = true;
            }

            // GPU VRAM.
            if (renderer)
            {
                vramTotalMB = static_cast<float>(renderer->vram_total_bytes()) / (1024.0f * 1024.0f);
                vramUsedMB = static_cast<float>(renderer->vram_used_bytes()) / (1024.0f * 1024.0f);
            }
        }

        void push_frametime(float dt)
        {
            const float ms = dt * 1000.0f;
            frametimeHistory[historyIndex] = ms;
            historyIndex = (historyIndex + 1) % kHistorySize;
            frametimeMs = ms;
            float sum = 0.0f, minV = 999.0f, maxV = 0.0f;
            for (auto v : frametimeHistory)
            {
                if (v <= 0.0f) continue;
                sum += v;
                minV = std::min(minV, v);
                maxV = std::max(maxV, v);
            }
            frametimeAvg = sum / static_cast<float>(kHistorySize);
            frametimeMin = minV;
            frametimeMax = maxV;
            const float instantFps = dt > 0.0001f ? 1.0f / dt : 0.0f;
            fpsSmoothed = fpsSmoothed * 0.92f + instantFps * 0.08f;
            systemUpdateTimer += dt;
            if (systemUpdateTimer >= 0.5f)
            {
                systemUpdateTimer = 0.0f;
                update_system_metrics();
            }
        }
    };

    void draw_perf_hud(PerfHudState& hud, float surfaceWidth)
    {
        if (!hud.visible)
            return;

        const float hudWidth = 240.0f;
        ImGui::SetNextWindowPos(ImVec2(surfaceWidth - hudWidth - 8.0f, 8.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(hudWidth, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.78f);

        const auto flags = ImGuiWindowFlags_NoDecoration
            | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoNav
            | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_NoMove;

        if (!ImGui::Begin("##PerfHud", nullptr, flags))
        {
            ImGui::End();
            return;
        }

        const auto colorForPercent = [](float pct) -> ImVec4 {
            if (pct < 60.0f) return { 0.2f, 1.0f, 0.4f, 1.0f };
            if (pct < 85.0f) return { 1.0f, 0.85f, 0.2f, 1.0f };
            return { 1.0f, 0.3f, 0.3f, 1.0f };
        };

        // FPS + frametime.
        const auto fpsColor = hud.fpsSmoothed >= 60.0f ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f)
            : hud.fpsSmoothed >= 30.0f ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f)
            : ImVec4(1.0f, 0.3f, 0.3f, 1.0f);
        ImGui::TextColored(fpsColor, "FPS: %.0f", hud.fpsSmoothed);
        ImGui::SameLine(130.0f);
        ImGui::Text("%.2f ms", hud.frametimeMs);

        // Frametime graph.
        ImGui::PlotLines("##ft", hud.frametimeHistory, static_cast<int>(PerfHudState::kHistorySize),
            static_cast<int>(hud.historyIndex), nullptr, 0.0f, std::max(16.7f, hud.frametimeMax * 1.2f),
            ImVec2(hudWidth - 16.0f, 36.0f));
        ImGui::Text("%.1f / %.1f / %.1f ms", hud.frametimeMin, hud.frametimeAvg, hud.frametimeMax);

        ImGui::Separator();

        // CPU overall.
        ImGui::TextColored(colorForPercent(hud.cpuPercent), "CPU: %.0f%%", hud.cpuPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%u cores", hud.cpuCores);

        // Per-core usage bars (compact: one row of colored blocks).
        {
            auto* drawList = ImGui::GetWindowDrawList();
            const auto cursor = ImGui::GetCursorScreenPos();
            const float barWidth = (hudWidth - 20.0f) / static_cast<float>(hud.cpuCores);
            const float barHeight = 10.0f;
            for (std::uint32_t i = 0; i < hud.cpuCores; ++i)
            {
                const float x = cursor.x + static_cast<float>(i) * barWidth;
                const float usage = std::clamp(hud.coreUsage[i] / 100.0f, 0.0f, 1.0f);
                // Background.
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    IM_COL32(40, 40, 40, 200));
                // Fill based on usage.
                const auto r = static_cast<std::uint8_t>(std::min(255.0f, usage * 2.0f * 255.0f));
                const auto g = static_cast<std::uint8_t>(std::min(255.0f, (1.0f - usage) * 2.0f * 255.0f));
                drawList->AddRectFilled(
                    ImVec2(x, cursor.y + barHeight * (1.0f - usage)),
                    ImVec2(x + barWidth - 1.0f, cursor.y + barHeight),
                    IM_COL32(r, g, 60, 220));
            }
            ImGui::Dummy(ImVec2(hudWidth - 16.0f, barHeight + 2.0f));
        }

        ImGui::Separator();

        // GPU.
        if (!hud.gpuName.empty())
            ImGui::TextDisabled("%s", hud.gpuName.c_str());
        if (hud.vramTotalMB > 0.0f)
        {
            const float vramPercent = (hud.vramUsedMB / hud.vramTotalMB) * 100.0f;
            ImGui::TextColored(colorForPercent(vramPercent), "VRAM: %.0f%%", vramPercent);
            ImGui::SameLine(130.0f);
            ImGui::Text("%.0f/%.0f MB", hud.vramUsedMB, hud.vramTotalMB);
        }

        ImGui::Separator();

        // RAM.
        ImGui::TextColored(colorForPercent(hud.ramPercent), "RAM: %.0f%%", hud.ramPercent);
        ImGui::SameLine(130.0f);
        ImGui::Text("%.1f/%.1f GB", hud.ramUsedMB / 1024.0f, hud.ramTotalMB / 1024.0f);
        ImGui::Text("Process: %.0f MB", hud.processRamMB);

        ImGui::Separator();

        // Scene stats.
        ImGui::Text("Batches: %u  Inst: %u", hud.visibleBatches, hud.visibleInstances);
        ImGui::Text("Tris: %uk  Actors: %u", hud.totalTriangles / 1000u, hud.actorCount);
        if (hud.mobsMoving > 0)
            ImGui::Text("Mobs moving: %u", hud.mobsMoving);

        ImGui::End();
    }

    struct UnifiedPanelResult
    {
        bool loadRequested{};
        bool viewDistanceChanged{};
        bool debugGizmosChanged{};
        bool characterApplyRequested{};
        bool weatherChanged{};
    };

    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::VulkanRenderer& renderer,
        bool& fogEnabled,
        bool& showSoundGizmos,
        bool& showMusicGizmos,
        bool& showPortalGizmos,
        bool& showEffectGizmos,
        bool& showNamePlates,
        bool& showCollisionDebug,
        bool& playMapSounds,
        bool& playMapMusic,
        int& selectedMapIndex,
        float& viewDistance,
        float& actorViewDistance,
        WeatherMode& weatherMode,
        const std::vector<CharacterOption>& characterOptions,
        int& selectedCharacterOption,
        phoenix::character::CharacterAppearance& appearance,
        float camX, float camY, float camZ, float camYaw, float camPitch)
    {
        UnifiedPanelResult result{};

        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Phoenix Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return result;
        }

        // ---- World Map ----
        const auto& maps = runtime.world_map_names();
        if (!maps.empty())
        {
            selectedMapIndex = std::clamp(selectedMapIndex, 0, static_cast<int>(maps.size() - 1));
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::BeginCombo("##map", maps[static_cast<std::size_t>(selectedMapIndex)].c_str()))
            {
                for (std::size_t i = 0; i < maps.size(); ++i)
                {
                    if (ImGui::Selectable(maps[i].c_str(), selectedMapIndex == static_cast<int>(i)))
                        selectedMapIndex = static_cast<int>(i);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            result.loadRequested = ImGui::Button("Load");
        }

        // ---- View settings ----
        const auto previousFog = fogEnabled;
        ImGui::Checkbox("Fog", &fogEnabled);
        if (fogEnabled != previousFog)
            apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);

        const auto previousViewDistance = viewDistance;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("View", &viewDistance, 100.0f, 2500.0f, "%.0f");
        result.viewDistanceChanged = std::abs(previousViewDistance - viewDistance) > 1.0f;

        const auto previousActorViewDistance = actorViewDistance;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Actors", &actorViewDistance, 10.0f, 2500.0f, "%.0f");
        result.viewDistanceChanged = result.viewDistanceChanged
            || std::abs(previousActorViewDistance - actorViewDistance) > 1.0f;

        const WeatherMode previousWeatherMode = weatherMode;
        const char* weatherItems[] = { "Default", "Storm", "Snowstorm", "Sunset", "Night" };
        int weatherIndex = static_cast<int>(weatherMode);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Weather", &weatherIndex, weatherItems, IM_ARRAYSIZE(weatherItems)))
            weatherMode = static_cast<WeatherMode>(std::clamp(weatherIndex, 0, 4));
        result.weatherChanged = weatherMode != previousWeatherMode;

        // ---- Debug overlays ----
        if (ImGui::TreeNodeEx("Overlays", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool prevSounds = showSoundGizmos;
            const bool prevMusic = showMusicGizmos;
            const bool prevPortals = showPortalGizmos;
            const bool prevEffects = showEffectGizmos;
            const bool prevCollision = showCollisionDebug;
            ImGui::Checkbox("Sounds", &showSoundGizmos);
            ImGui::SameLine();
            ImGui::Checkbox("Music", &showMusicGizmos);
            ImGui::Checkbox("Portals", &showPortalGizmos);
            ImGui::SameLine();
            ImGui::Checkbox("Effects", &showEffectGizmos);
            ImGui::Checkbox("Names", &showNamePlates);
            ImGui::SameLine();
            ImGui::Checkbox("Collision", &showCollisionDebug);
            ImGui::Checkbox("Play Sounds", &playMapSounds);
            ImGui::SameLine();
            ImGui::Checkbox("Play Music", &playMapMusic);
            result.debugGizmosChanged = prevSounds != showSoundGizmos
                || prevMusic != showMusicGizmos
                || prevPortals != showPortalGizmos
                || prevEffects != showEffectGizmos
                || prevCollision != showCollisionDebug;
            ImGui::TreePop();
        }

        // ---- Character ----
        if (!characterOptions.empty() && ImGui::TreeNodeEx("Character", ImGuiTreeNodeFlags_DefaultOpen))
        {
            selectedCharacterOption = std::clamp(selectedCharacterOption, 0, static_cast<int>(characterOptions.size() - 1));
            const auto& selected = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo("Model", selected.label.c_str()))
            {
                for (std::size_t i = 0; i < characterOptions.size(); ++i)
                {
                    const bool isSelected = selectedCharacterOption == static_cast<int>(i);
                    if (ImGui::Selectable(characterOptions[i].label.c_str(), isSelected))
                    {
                        selectedCharacterOption = static_cast<int>(i);
                        appearance.raceFolder = characterOptions[i].raceFolder;
                        appearance.prefix = characterOptions[i].prefix;
                        appearance.armorIndex = nearest_available(appearance.armorIndex, characterOptions[i].armorIndices);
                        appearance.faceIndex = nearest_available(appearance.faceIndex, characterOptions[i].faceIndices);
                        appearance.hairIndex = nearest_available(appearance.hairIndex, characterOptions[i].hairIndices);
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const auto& current = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
            appearance.armorIndex = nearest_available(appearance.armorIndex, current.armorIndices);
            appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
            appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Armor", &appearance.armorIndex);
            appearance.armorIndex = nearest_available(appearance.armorIndex, current.armorIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Face", &appearance.faceIndex);
            appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Hair", &appearance.hairIndex);
            appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);

            result.characterApplyRequested = ImGui::Button("Apply");
            ImGui::TreePop();
        }

        // ---- Coordinates ----
        ImGui::Separator();
        ImGui::Text("X: %.1f  Y: %.1f  Z: %.1f", camX, camY, camZ);
        ImGui::Text("Yaw: %.2f  Pitch: %.2f", camYaw, camPitch);

        ImGui::End();
        return result;
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    const auto executableDir = executable_directory();
    {
        std::ofstream log(kLogName, std::ios::trunc);
        log << "Phoenix Engine\n";
        log << "Executable directory: " << executableDir.string() << "\n";
    }

    phoenix::platform::Win32Window window;
    if (!window.create(instance, kWidth, kHeight, kAppTitle))
    {
        MessageBoxW(nullptr, L"Could not create Phoenix Engine window.", kAppTitle, MB_ICONERROR);
        return 1;
    }

    const auto [clientWidth, clientHeight] = window.client_size();
    phoenix::renderer::VulkanRenderer renderer;
    if (!renderer.initialize(
        window.handle(),
        static_cast<std::uint32_t>(clientWidth > 0 ? clientWidth : kWidth),
        static_cast<std::uint32_t>(clientHeight > 0 ? clientHeight : kHeight)))
    {
        MessageBoxW(window.handle(), L"Could not initialize Vulkan.", kAppTitle, MB_ICONERROR);
        return 1;
    }

    const LoadingIcon loadingIcon{
        phoenix::generated::kLoadingIconWidth,
        phoenix::generated::kLoadingIconHeight,
        phoenix::generated::kLoadingIconBgra};

    const auto imguiAvailable = renderer.initialize_imgui(window.handle());
    if (!imguiAvailable)
    {
        std::ofstream log(kLogName, std::ios::app);
        log << "ImGui initialization unavailable\n";
    }

    auto showLoading = [&](float progress, std::wstring_view stage) {
        window.pump_messages();
        window.set_title(std::wstring(kAppTitle) + L" - Loading - " + std::wstring(stage));
        const auto width = std::max(1u, renderer.surface_width());
        const auto height = std::max(1u, renderer.surface_height());
        auto image = make_loading_image(width, height, progress, loadingIcon);
        renderer.set_preview_image(width, height, image);
        renderer.render_frame();
    };

    showLoading(0.03f, L"Starting");

    phoenix::runtime::PhoenixRuntime runtime;
    runtime.initialize(executableDir, false);
    showLoading(0.12f, L"Indexing data");

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
    {
        showLoading(0.17f, L"Loading world");
        runtime.load_world_map(defaultMap);
    }
    showLoading(0.24f, L"World ready");

    phoenix::audio::AudioSystem audioSystem;
    const bool audioAvailable = audioSystem.initialize();
    showLoading(0.27f, L"Audio");
    if (!audioAvailable)
    {
        std::ofstream log(kLogName, std::ios::app);
        log << "Audio initialization unavailable\n";
    }

    phoenix::character::CharacterSystem characterSystem;
    phoenix::character::CharacterAppearance characterAppearance{};
    auto characterOptions = scan_character_options(runtime.state().assets.root);
    characterSystem.preload(runtime.state().assets.root);
    showLoading(0.32f, L"Characters");
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
    bool playableMode = false; // true = third-person playable, false = free camera viewer

    bool fogEnabled = true;
    float viewDistance = 1000.0f;
    float actorViewDistance = 100.0f;
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
    std::vector<phoenix::renderer::DdsTexture> cachedTextures;
    std::size_t characterTextureBaseSlot = 0;
    phoenix::runtime::StaticObjectScene staticObjectScene;
    phoenix::runtime::AnimatedObjectScene animatedObjectScene;
    phoenix::world::ActorScene actorScene;
    std::size_t actorBatchStart = std::numeric_limits<std::size_t>::max();
    std::size_t actorAnimatedBatchStart = std::numeric_limits<std::size_t>::max();
    std::size_t actorVertexAnimationStart = std::numeric_limits<std::size_t>::max();
    phoenix::runtime::WorldCollisionMesh worldCollisionMesh;
    HeightSamplerContext heightSamplerCtx{ &runtime, &worldCollisionMesh };
    MapAudioScene mapAudioScene;
    bool forceVisibilityUpdate = true;
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

    const auto reloadCharacterIntoRenderer = [&]() {
        if (characterTextureBaseSlot == 0 || cachedTextures.empty())
            return false;

        characterLoaded = characterSystem.load(runtime.state().assets.root, characterAppearance);
        if (!characterLoaded)
            return false;

        characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
        characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
        const auto& charTexPaths = characterSystem.texture_paths();
        cachedTextures.resize(characterTextureBaseSlot + charTexPaths.size());
        for (std::size_t i = 0; i < charTexPaths.size(); ++i)
            cachedTextures[characterTextureBaseSlot + i] = phoenix::renderer::load_dds(charTexPaths[i]);
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));
        renderer.upload_terrain_textures(cachedTextures);
        uploadCharacterMesh();
        return true;
    };

    const auto uploadCurrentWorld = [&]() {
        showLoading(0.36f, L"Preparing scene");
        applyFogSettings();
        mapAudioScene = build_map_audio_scene(runtime);
        {
            std::ofstream audioLog(kLogName, std::ios::app);
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
        showLoading(0.38f, L"Loading actors");
        actorScene = phoenix::world::build_actor_scene(
            runtime.state().dataRoot,
            mapStem,
            runtime.state().assets,
            static_cast<std::uint32_t>(actorTextureBaseSlot),
            character_height_sampler,
            &heightSamplerCtx);
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
                showLoading(0.40f + textureProgress * 0.26f, L"Loading textures");
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
            for (auto& t : threads)
                t.join();
            showLoading(0.66f, L"Textures ready");
        }


        {
            std::ofstream textureLog(kLogName, std::ios::app);
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
        if (!characterLoaded)
        {
            characterLoaded = characterSystem.load(runtime.state().assets.root, characterAppearance);
            if (characterLoaded)
            {
                characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
                characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
                const auto& charTexPaths = characterSystem.texture_paths();
                const auto newTotalSlots = characterTextureBaseSlot + charTexPaths.size();
                terrainTextures.resize(newTotalSlots);
                for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                    terrainTextures[characterTextureBaseSlot + i] = phoenix::renderer::load_dds(charTexPaths[i]);
                characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));

                std::ofstream log(kLogName, std::ios::app);
                log << "Character textures base layer: " << characterTextureBaseSlot
                    << " count: " << charTexPaths.size() << "\n";
            }
        }
        else if (characterLoaded)
        {
            const auto& charTexPaths = characterSystem.texture_paths();
            const auto newTotalSlots = characterTextureBaseSlot + charTexPaths.size();
            terrainTextures.resize(newTotalSlots);
            for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                terrainTextures[characterTextureBaseSlot + i] = phoenix::renderer::load_dds(charTexPaths[i]);
            characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));
        }

        showLoading(0.68f, L"Uploading textures");
        if (!terrainTextures.empty())
        {
            renderer.upload_terrain_textures(terrainTextures);
            cachedTextures = std::move(terrainTextures);
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

        showLoading(0.74f, L"Building terrain");
        runtime.build_terrain_mesh(terrainVertices, terrainIndices);
        terrainVertexCount = static_cast<std::uint32_t>(terrainVertices.size());
        terrainIndexCount = static_cast<std::uint32_t>(terrainIndices.size());
        {
            std::ofstream log(kLogName, std::ios::app);
            log << "Terrain mesh: vertices=" << terrainVertices.size()
                << " indices=" << terrainIndices.size() << "\n";
        }

        if (!renderer.set_terrain_mesh(terrainVertices, terrainIndices))
        {
            std::ofstream log(kLogName, std::ios::app);
            log << "Terrain mesh upload unavailable; using CPU preview fallback\n";
            const auto previewWidth = std::max(480u, renderer.surface_width() / 2u);
            const auto previewHeight = std::max(270u, renderer.surface_height() / 2u);
            auto preview = runtime.create_3d_preview_image(previewWidth, previewHeight);
            renderer.set_preview_image(preview.width, preview.height, preview.bgra);
        }

        showLoading(0.82f, L"Building objects");
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
                animation.idleFrames = actorAnimation.idleFrames;
                animation.walkFrames = actorAnimation.walkFrames;
                animation.runFrames = actorAnimation.runFrames;
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
        }
        actorBatchStart = staticObjectScene.batches.size();
        if (!actorScene.vertices.empty() && !actorScene.indices.empty() && !actorScene.instances.empty())
        {
            const auto vertexBase = static_cast<std::uint32_t>(staticObjectScene.vertices.size());
            const auto indexBase = static_cast<std::uint32_t>(staticObjectScene.indices.size());
            const auto instanceBase = static_cast<std::uint32_t>(staticObjectScene.instances.size());
            staticObjectScene.vertices.insert(staticObjectScene.vertices.end(), actorScene.vertices.begin(), actorScene.vertices.end());
            staticObjectScene.indices.reserve(staticObjectScene.indices.size() + actorScene.indices.size());
            for (const auto index : actorScene.indices)
                staticObjectScene.indices.push_back(vertexBase + index);
            staticObjectScene.instances.insert(staticObjectScene.instances.end(), actorScene.instances.begin(), actorScene.instances.end());
            for (auto batch : actorScene.batches)
            {
                batch.firstIndex += indexBase;
                batch.firstInstance += instanceBase;
                staticObjectScene.batches.push_back(batch);
            }
            for (const auto& bounds : actorScene.batchBounds)
            {
                phoenix::runtime::StaticObjectScene::BatchBounds converted{};
                converted.x = bounds.x;
                converted.y = bounds.y;
                converted.z = bounds.z;
                converted.radius = bounds.radius;
                staticObjectScene.batchBounds.push_back(converted);
            }
        }
        objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
        objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
        {
            std::ofstream log(kLogName, std::ios::app);
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
            std::ofstream log(kLogName, std::ios::app);
            log << "Static object mesh upload unavailable\n";
        }

        if (!animatedObjectScene.vertices.empty()
            && !renderer.set_animated_object_mesh(
                animatedObjectScene.vertices,
                animatedObjectScene.indices,
                animatedObjectScene.instances,
                animatedObjectScene.batches))
        {
            std::ofstream log(kLogName, std::ios::app);
            log << "Animated object mesh upload unavailable\n";
        }

        showLoading(0.92f, L"Finalizing scene");
        uploadDebugGizmos();

        // Build collision mesh from world objects.
        worldCollisionMesh = runtime.build_collision_mesh();

        // Re-sample actor heights now that collision mesh is available.
        // Actors were initially placed using terrain-only height; now they
        // should also stand on walkable collision surfaces (bridges, etc.).
        {
            // Static actor instances (mobs).
            if (actorBatchStart < staticObjectScene.batches.size())
            {
                const auto firstActorInstance = staticObjectScene.batches[actorBatchStart].firstInstance;
                for (std::size_t i = firstActorInstance; i < staticObjectScene.instances.size(); ++i)
                {
                    auto& inst = staticObjectScene.instances[i];
                    const float x = inst.position[0];
                    const float z = inst.position[2];
                    heightSamplerCtx.lastCharacterY = inst.position[1];
                    inst.position[1] = character_height_sampler(x, z, &heightSamplerCtx);
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

        // Upload character mesh (initial bind pose).
        if (characterLoaded && characterSystem.ready())
            uploadCharacterMesh();

        forceVisibilityUpdate = true;
    };

    uploadCurrentWorld();
    runtime.update_window_title(window.handle(), renderer.adapter_name(), displayedFps, fogEnabled);

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
            {
                float camX, camY, camZ, camYaw, camPitch;
                if (playableMode && characterSystem.ready())
                    characterSystem.camera_state(camX, camY, camZ, camYaw, camPitch);
                else
                    runtime.camera_state(camX, camY, camZ, camYaw, camPitch);
                runtime.update_animated_object_scene(animatedObjectScene, totalTime, deltaSeconds, camX, camY, camZ, actorVertexAnimationStart);
            }
            renderer.update_animated_object_scene(animatedObjectScene.vertices, animatedObjectScene.instances);
        }

        const auto fogToggleDown = window.is_key_down('F');
        if (fogToggleDown && !fogToggleWasDown)
        {
            fogEnabled = !fogEnabled;
            applyFogSettings();
        }
        fogToggleWasDown = fogToggleDown;

        // Toggle playable mode with P key.
        const auto playToggleDown = window.is_key_down('P');
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
                    std::ofstream log(kLogName, std::ios::app);
                    log << "Playable dungeon spawn: x=" << spawn.x
                        << " y=" << spawn.y
                        << " z=" << spawn.z
                        << " camera=(" << freeCamX << "," << freeCamY << "," << freeCamZ << ")\n";
                }
                else
                {
                    std::ofstream log(kLogName, std::ios::app);
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
                pInput.forward = window.is_key_down('W');
                pInput.backward = window.is_key_down('S');
                pInput.left = window.is_key_down('A');
                pInput.right = window.is_key_down('D');
                pInput.jump = window.is_key_down(VK_SPACE);
                pInput.fast = window.is_key_down(VK_SHIFT);
                pInput.yawLeft = window.is_key_down(VK_LEFT);
                pInput.yawRight = window.is_key_down(VK_RIGHT);
                pInput.pitchUp = window.is_key_down(VK_UP);
                pInput.pitchDown = window.is_key_down(VK_DOWN);
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
                cameraInput.forward = window.is_key_down('W');
                cameraInput.backward = window.is_key_down('S');
                cameraInput.left = window.is_key_down('A');
                cameraInput.right = window.is_key_down('D');
                cameraInput.up = window.is_key_down('E') || window.is_key_down(VK_SPACE);
                cameraInput.down = window.is_key_down('Q') || window.is_key_down(VK_CONTROL);
                cameraInput.fast = window.is_key_down(VK_SHIFT);
                cameraInput.yawLeft = window.is_key_down(VK_LEFT);
                cameraInput.yawRight = window.is_key_down(VK_RIGHT);
                cameraInput.pitchUp = window.is_key_down(VK_UP);
                cameraInput.pitchDown = window.is_key_down(VK_DOWN);
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

            std::vector<phoenix::renderer::ObjectBatch> visibleBatches;
            build_visible_object_batches(staticObjectScene, currentView, actorBatchStart, actorViewDistance, visibleBatches);
            renderer.set_static_object_batches(visibleBatches);
            std::uint32_t visibleObjectInstances{};
            for (const auto& batch : visibleBatches)
                visibleObjectInstances += batch.instanceCount;
            objectInstanceCount = visibleObjectInstances;
            objectBatchCount = static_cast<std::uint32_t>(visibleBatches.size());

            if (!animatedObjectScene.batches.empty())
            {
                std::vector<phoenix::renderer::ObjectBatch> visibleAnimatedBatches;
                build_visible_animated_batches(animatedObjectScene, currentView, actorAnimatedBatchStart, actorViewDistance, visibleAnimatedBatches);
                renderer.set_animated_object_batches(visibleAnimatedBatches);
            }

            lastCullView = currentView;
            forceVisibilityUpdate = false;
        }

        if (imguiAvailable)
        {
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
                weatherMode,
                characterOptions,
                selectedCharacterOption,
                characterAppearance,
                cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);

            if (panelResult.loadRequested
                && runtime.load_world_map(static_cast<std::size_t>(std::max(0, selectedMapIndex))))
            {
                applyFogSettings();
                uploadCurrentWorld();
            }
            else if (panelResult.viewDistanceChanged)
            {
                applyFogSettings();
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

            if (panelResult.characterApplyRequested)
                reloadCharacterIntoRenderer();

            if (showNamePlates && !actorScene.labels.empty())
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

        renderer.render_frame();

        if (now - lastTitleUpdate > std::chrono::seconds(1))
        {
            const auto titleSeconds = std::chrono::duration<float>(now - lastTitleUpdate).count();
            displayedFps = titleSeconds > 0.0f ? static_cast<float>(framesSinceTitleUpdate) / titleSeconds : 0.0f;
            framesSinceTitleUpdate = 0;
            runtime.update_window_title(window.handle(), renderer.adapter_name(), displayedFps, fogEnabled);
            lastTitleUpdate = now;
        }
    }

    renderer.shutdown();
    audioSystem.shutdown();
    return 0;
}
