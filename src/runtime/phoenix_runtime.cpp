#define _CRT_SECURE_NO_WARNINGS
#include "runtime/phoenix_runtime.h"

#include "assets/data_index.h"
#include "renderer/dds_loader.h"
#include "world/dg_loader.h"
#include "world/smod_loader.h"
#include "world/vani_loader.h"
#include "world/mani_loader.h"
#include "world/wld_loader.h"
#include "world/water_constants.h"
#include "world/coordinate_conversion.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace phoenix::runtime
{
    namespace
    {
        constexpr std::uint32_t kAssetTextureLayerBase = 66;
        constexpr std::uint32_t kAssetCutoutLayerBase = 2048;
        constexpr float kManiTicksPerSecond = 30.0f;

        inline std::filesystem::path resolve_ci(const std::filesystem::path& path)
        {
            return assets::resolve_existing_path_case_insensitive(path);
        }

        bool is_world_asset_extension(std::string extension)
        {
            extension = phoenix::assets::lower_ascii(std::move(extension));
            return extension == ".smod" || extension == ".dg" || extension == ".vani";
        }

        bool is_audio_asset_extension(std::string extension)
        {
            extension = phoenix::assets::lower_ascii(std::move(extension));
            return extension == ".ogg";
        }

        void put_pixel(PreviewImage& image, int x, int y, std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            if (x < 0 || y < 0 || x >= static_cast<int>(image.width) || y >= static_cast<int>(image.height))
                return;

            const auto offset = (static_cast<std::size_t>(y) * image.width + static_cast<std::size_t>(x)) * 4;
            image.bgra[offset + 0] = b;
            image.bgra[offset + 1] = g;
            image.bgra[offset + 2] = r;
            image.bgra[offset + 3] = 255;
        }

        void draw_dot(PreviewImage& image, int cx, int cy, int radius, std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            for (int y = -radius; y <= radius; ++y)
            {
                for (int x = -radius; x <= radius; ++x)
                {
                    if (x * x + y * y <= radius * radius)
                        put_pixel(image, cx + x, cy + y, r, g, b);
                }
            }
        }

        void draw_line(PreviewImage& image, int x0, int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            const auto dx = std::abs(x1 - x0);
            const auto sx = x0 < x1 ? 1 : -1;
            const auto dy = -std::abs(y1 - y0);
            const auto sy = y0 < y1 ? 1 : -1;
            auto error = dx + dy;

            for (;;)
            {
                put_pixel(image, x0, y0, r, g, b);
                if (x0 == x1 && y0 == y1)
                    break;

                const auto e2 = error * 2;
                if (e2 >= dy)
                {
                    error += dy;
                    x0 += sx;
                }
                if (e2 <= dx)
                {
                    error += dx;
                    y0 += sy;
                }
            }
        }

        struct ProjectedPoint
        {
            int x{};
            int y{};
            float depth{};
        };

        std::uint32_t color_hash(std::string_view value)
        {
            std::uint32_t hash = 2166136261u;
            for (const auto ch : value)
            {
                hash ^= static_cast<std::uint8_t>(ch);
                hash *= 16777619u;
            }
            return hash;
        }

        // Build a renderer instance from a scene object: orthonormal basis from
        // the stored forward/up, MANI rotation axis*speed packed into the .w
        // components, and the distance-cull flag in position.w.
        phoenix::renderer::ObjectInstance make_object_instance(
            const phoenix::runtime::SceneObject& object, float cullDistance)
        {
            const auto normalize = [](float* vector, const float* fallback) {
                const auto length = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
                if (length < 0.001f)
                {
                    vector[0] = fallback[0];
                    vector[1] = fallback[1];
                    vector[2] = fallback[2];
                    return;
                }
                vector[0] /= length;
                vector[1] /= length;
                vector[2] /= length;
            };

            float forward[3]{ object.forward[0], object.forward[1], object.forward[2] };
            float up[3]{ object.up[0], object.up[1], object.up[2] };
            const float fallbackForward[3]{ 0.0f, 0.0f, 1.0f };
            const float fallbackUp[3]{ 0.0f, 1.0f, 0.0f };
            normalize(forward, fallbackForward);
            normalize(up, fallbackUp);

            const float right[3]{
                up[1] * forward[2] - up[2] * forward[1],
                up[2] * forward[0] - up[0] * forward[2],
                up[0] * forward[1] - up[1] * forward[0],
            };

            float axisSpeed[3]{};
            if (std::abs(object.maniRotationSpeed) >= 0.0001f)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    axisSpeed[axis] =
                        right[axis] * object.maniRotationAxis[0]
                        + up[axis] * object.maniRotationAxis[1]
                        + forward[axis] * object.maniRotationAxis[2];
                }
                const auto axisLength = std::sqrt(
                    axisSpeed[0] * axisSpeed[0]
                    + axisSpeed[1] * axisSpeed[1]
                    + axisSpeed[2] * axisSpeed[2]);
                if (axisLength >= 0.001f)
                {
                    const auto scale = object.maniRotationSpeed / axisLength;
                    axisSpeed[0] *= scale;
                    axisSpeed[1] *= scale;
                    axisSpeed[2] *= scale;
                }
                else
                {
                    axisSpeed[0] = 0.0f;
                    axisSpeed[1] = 0.0f;
                    axisSpeed[2] = 0.0f;
                }
            }

            phoenix::renderer::ObjectInstance instance{};
            instance.right[0] = right[0];
            instance.right[1] = right[1];
            instance.right[2] = right[2];
            instance.right[3] = axisSpeed[0];
            instance.up[0] = up[0];
            instance.up[1] = up[1];
            instance.up[2] = up[2];
            instance.up[3] = axisSpeed[1];
            instance.forward[0] = forward[0];
            instance.forward[1] = forward[1];
            instance.forward[2] = forward[2];
            instance.forward[3] = axisSpeed[2];
            instance.position[0] = object.x;
            instance.position[1] = object.y;
            instance.position[2] = object.z;
            instance.position[3] = cullDistance; // 0 = no cull, >0 = max render distance
            return instance;
        }

        float normal_light(const float* normal)
        {
            const float light[3]{ -0.32f, 0.72f, -0.61f };
            const auto dot = normal[0] * light[0] + normal[1] * light[1] + normal[2] * light[2];
            return std::clamp(0.58f + dot * 0.30f, 0.34f, 1.0f);
        }

        void append_preview_vertex(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            const float* position,
            const float* normal,
            const float* uv,
            std::uint32_t materialHash,
            std::uint32_t textureLayer)
        {
            const auto tint = static_cast<float>(materialHash & 0xFFu) / 255.0f;
            const auto light = normal_light(normal);
            phoenix::renderer::TerrainVertex vertex{};
            vertex.position[0] = position[0];
            vertex.position[1] = position[1];
            vertex.position[2] = position[2];
            vertex.color[0] = (0.43f + tint * 0.23f) * light;
            vertex.color[1] = (0.39f + (1.0f - tint) * 0.20f) * light;
            vertex.color[2] = (0.31f + static_cast<float>((materialHash >> 8) & 0x7Fu) / 720.0f) * light;
            vertex.normal[0] = normal[0];
            vertex.normal[1] = normal[1];
            vertex.normal[2] = normal[2];
            vertex.uv[0] = uv ? uv[0] : 0.0f;
            vertex.uv[1] = uv ? uv[1] : 0.0f;
            vertex.textureLayer = textureLayer;
            vertices.push_back(vertex);
        }

        // Dungeon variant: vertex color carries lightmap UV + page index using
        // the encoding the static_object pixel shader expects (color.rg = UV,
        // color.b = page + 2.0 — values >= 1.5 select the lightmap path).
        void append_preview_vertex_lightmapped(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            const float* position,
            const float* normal,
            const float* uv,
            const float* lightmapUv,
            std::int32_t lightmapPage,
            std::uint32_t textureLayer)
        {
            phoenix::renderer::TerrainVertex vertex{};
            vertex.position[0] = position[0];
            vertex.position[1] = position[1];
            vertex.position[2] = position[2];
            vertex.color[0] = lightmapUv[0];
            vertex.color[1] = lightmapUv[1];
            vertex.color[2] = static_cast<float>(lightmapPage) + 2.0f;
            vertex.normal[0] = normal[0];
            vertex.normal[1] = normal[1];
            vertex.normal[2] = normal[2];
            vertex.uv[0] = uv ? uv[0] : 0.0f;
            vertex.uv[1] = uv ? uv[1] : 0.0f;
            vertex.textureLayer = textureLayer;
            vertices.push_back(vertex);
        }

        std::vector<std::filesystem::path> terrain_detail_paths_for_map(
            const phoenix::assets::DataIndex& assets,
            const phoenix::world::WldAnalysis& world)
        {
            std::vector<std::filesystem::path> paths;
            paths.reserve(8);
            for (const auto& layer : world.terrainLayers)
            {
                auto path = phoenix::assets::resolve_texture_asset(assets, layer.textureFileName);
                paths.push_back(std::move(path));
            }

            if (paths.size() > 8)
                paths.resize(8);
            return paths;
        }
    }

    bool PhoenixRuntime::initialize(const std::filesystem::path& executableDir, bool loadDefaultMap)
    {
        state_.dataRoot = find_data_root(executableDir);
        // Placeable entities live in data/entity/ (renamed from Assets/).
        state_.entityRoot = assets::resolve_existing_path_case_insensitive(state_.dataRoot / "entity");
        if (state_.entityRoot.empty() || !std::filesystem::exists(state_.entityRoot))
            state_.entityRoot = assets::resolve_existing_path_case_insensitive(state_.dataRoot / "Assets");
        state_.assets = phoenix::assets::index_data_directory(state_.dataRoot);
        scan_entity_assets();
        scan_world_maps();
        scan_sky_assets();
        scan_terrain_textures();
        scan_audio_assets();

        std::size_t defaultMap{};
        for (std::size_t i = 0; i < state_.worldMapPaths.size(); ++i)
        {
            const auto stem = state_.worldMapPaths[i].stem().string();
            char* end = nullptr;
            const long n = std::strtol(stem.c_str(), &end, 10);
            if (end != stem.c_str() && *end == '\0' && n == 1)
            {
                defaultMap = i;
                break;
            }
        }
        if (loadDefaultMap && !state_.worldMapPaths.empty())
            load_world_map(defaultMap);
        else
            update_status();

        return true;
    }

    std::filesystem::path PhoenixRuntime::find_data_root(const std::filesystem::path& executableDir) const
    {
        // The data tree is all-lowercase ("data/world", "data/assets", ...).
        // Keep checking the legacy capitalised names too so external installs
        // (env override, AppData) don't break on case-sensitive filesystems.
        auto validDataRoot = [](const std::filesystem::path& path) {
            return std::filesystem::exists(path / "world")
                || std::filesystem::exists(path / "entity")
                || std::filesystem::exists(path / "character")
                || std::filesystem::exists(path / "World")
                || std::filesystem::exists(path / "Assets")
                || std::filesystem::exists(path / "Character");
        };

        std::vector<std::filesystem::path> candidates;
        if (const char* envValue = std::getenv("PHOENIX_ENGINE_DATA"); envValue && envValue[0])
            candidates.emplace_back(envValue);

        const std::filesystem::path parentDirs[] = {
            executableDir,
            std::filesystem::current_path(),
            executableDir.parent_path(),
            executableDir.parent_path().parent_path(),
            executableDir.parent_path().parent_path().parent_path(),
        };
        for (const auto& dir : parentDirs)
        {
            candidates.push_back(dir / "data");
            candidates.push_back(dir / "Data");
        }

#ifdef _WIN32
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && localAppData[0])
            candidates.emplace_back(std::filesystem::path(localAppData) / "Phoenix Engine" / "data");
        if (const char* programData = std::getenv("PROGRAMDATA"); programData && programData[0])
            candidates.emplace_back(std::filesystem::path(programData) / "Phoenix Engine" / "data");
#else
        if (const char* home = std::getenv("HOME"); home && home[0])
            candidates.emplace_back(std::filesystem::path(home) / ".local" / "share" / "Phoenix Engine" / "data");
#endif

        for (const auto& candidate : candidates)
            if (validDataRoot(candidate))
                return candidate;

        return candidates.empty() ? executableDir / "data" : candidates.front();
    }

    bool PhoenixRuntime::load_world_map(std::size_t mapIndex)
    {
        if (mapIndex >= state_.worldMapPaths.size())
            return false;

        state_.selectedWorldMap = mapIndex;
        const auto& wldPath = state_.worldMapPaths[mapIndex];
        const auto worldDir = wldPath.parent_path();
        state_.world = phoenix::world::analyze_wld(wldPath);

        // Set field directory for lightmap loading (World/field/<mapId>/).
        {
            const auto fieldDir = resolve_ci(worldDir / "field" / wldPath.stem());
            if (!fieldDir.empty() && std::filesystem::is_directory(fieldDir))
                state_.world.phoenixWorldFieldDir = fieldDir;
        }
        load_world_assets();
        load_effect_library();
        update_status();

        camera_ = {};
        if (state_.world.isDungeon && !state_.sceneObjects.empty())
        {
            // Compute XZ centroid from all scene objects.
            float sumX = 0.0f, sumZ = 0.0f;
            for (const auto& obj : state_.sceneObjects)
            {
                sumX += obj.x;
                sumZ += obj.z;
            }
            const auto count = static_cast<float>(state_.sceneObjects.size());
            const float centroidX = sumX / count;
            const float centroidZ = sumZ / count;

            // Collect Y values from objects near the centroid to find a valid floor level.
            // Using nearby objects avoids outliers from distant geometry.
            constexpr float kSearchRadius = 300.0f;
            std::vector<float> nearbyYValues;
            nearbyYValues.reserve(state_.sceneObjects.size());
            for (const auto& obj : state_.sceneObjects)
            {
                const float dx = obj.x - centroidX;
                const float dz = obj.z - centroidZ;
                if (dx * dx + dz * dz < kSearchRadius * kSearchRadius)
                    nearbyYValues.push_back(obj.y);
            }
            // Fallback: if nothing is near the centroid, use all objects.
            if (nearbyYValues.size() < 4)
            {
                nearbyYValues.clear();
                for (const auto& obj : state_.sceneObjects)
                    nearbyYValues.push_back(obj.y);
            }
            std::sort(nearbyYValues.begin(), nearbyYValues.end());

            // Use the 15th percentile as the floor level - low enough to be on
            // a walkable surface, but not the absolute min (could be below geometry).
            const auto floorIdx = std::min<std::size_t>(
                nearbyYValues.size() - 1,
                nearbyYValues.size() * 15 / 100);
            const float floorY = nearbyYValues[floorIdx];

            camera_.x = centroidX;
            camera_.y = floorY + 8.0f; // slightly above floor (eye height)
            camera_.z = centroidZ;
            camera_.pitch = 0.0f;
            camera_.speed = 60.0f;
        }

        return state_.world.parsed;
    }

    void PhoenixRuntime::scan_entity_assets()
    {
        state_.entityAssets.clear();
        if (!std::filesystem::exists(state_.entityRoot))
            return;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(state_.entityRoot))
        {
            if (!entry.is_regular_file() || !is_world_asset_extension(entry.path().extension().string()))
                continue;

            EntityAsset asset{};
            asset.path = entry.path();
            const auto relativePath = std::filesystem::relative(entry.path(), state_.entityRoot);
            asset.displayName = relativePath.string();
            // First path component is the WLD section (Building, Shape, Tree, Grass, etc.)
            if (relativePath.has_parent_path())
            {
                auto it = relativePath.begin();
                asset.section = it->string();
            }
            state_.entityAssets.push_back(std::move(asset));
        }

        // Dungeon assets live in World/dungeon/ (moved out of Assets/) but
        // still belong in the entity browser under the "dungeon" section.
        const auto dungeonRoot = resolve_ci(state_.dataRoot / "World" / "dungeon");
        if (!dungeonRoot.empty() && std::filesystem::is_directory(dungeonRoot))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(dungeonRoot))
            {
                if (!entry.is_regular_file() || !is_world_asset_extension(entry.path().extension().string()))
                    continue;
                EntityAsset asset{};
                asset.path = entry.path();
                const auto relativePath = std::filesystem::relative(entry.path(), dungeonRoot);
                asset.displayName = "dungeon/" + relativePath.string();
                asset.section = "dungeon";
                state_.entityAssets.push_back(std::move(asset));
            }
        }

        std::ranges::sort(state_.entityAssets, [](const auto& lhs, const auto& rhs) {
            const auto sl = phoenix::assets::lower_ascii(lhs.section);
            const auto sr = phoenix::assets::lower_ascii(rhs.section);
            if (sl != sr) return sl < sr;
            return phoenix::assets::lower_ascii(lhs.displayName) < phoenix::assets::lower_ascii(rhs.displayName);
        });
    }

    void PhoenixRuntime::scan_world_maps()
    {
        state_.worldMapPaths.clear();
        state_.worldMapNames.clear();

        const auto worldRoot = resolve_ci(state_.dataRoot / "World");
        if (!std::filesystem::exists(worldRoot))
            return;

        // Flat layout: all <id>.wld files live directly in World/, lightmaps
        // in World/field/<id>/, dungeon assets in World/dungeon/.
        for (const auto& entry : std::filesystem::directory_iterator(worldRoot))
        {
            if (!entry.is_regular_file())
                continue;
            const auto ext = phoenix::assets::lower_ascii(entry.path().extension().string());
            if (ext != ".wld")
                continue;
            state_.worldMapPaths.push_back(entry.path());
        }

        std::ranges::sort(state_.worldMapPaths, [](const auto& lhs, const auto& rhs) {
            const auto nameL = lhs.stem().string();
            const auto nameR = rhs.stem().string();
            char* endL = nullptr;
            char* endR = nullptr;
            const auto numL = std::strtol(nameL.c_str(), &endL, 10);
            const auto numR = std::strtol(nameR.c_str(), &endR, 10);
            const bool isNumL = endL != nameL.c_str() && *endL == '\0';
            const bool isNumR = endR != nameR.c_str() && *endR == '\0';
            if (isNumL && isNumR)
                return numL < numR;
            if (isNumL != isNumR)
                return isNumL;
            return nameL < nameR;
        });

        // Display names stay "world<id>" — the UI, portals and the default-map
        // selection all key off that convention.
        state_.worldMapNames.reserve(state_.worldMapPaths.size());
        for (const auto& path : state_.worldMapPaths)
            state_.worldMapNames.push_back("world" + path.stem().string());
    }

    void PhoenixRuntime::scan_sky_assets()
    {
        state_.skyFileNames.clear();
        state_.skyFileNames.push_back("");

        const auto skyRoot = resolve_ci(state_.dataRoot / "Sky");
        if (!std::filesystem::exists(skyRoot))
            return;

        for (const auto& entry : std::filesystem::directory_iterator(skyRoot))
        {
            if (!entry.is_regular_file())
                continue;

            const auto extension = phoenix::assets::lower_ascii(entry.path().extension().string());
            if (extension != ".dds")
                continue;

            state_.skyFileNames.push_back(entry.path().filename().string());
        }

        std::ranges::sort(state_.skyFileNames.begin() + 1, state_.skyFileNames.end(), [](const auto& lhs, const auto& rhs) {
            return phoenix::assets::lower_ascii(lhs) < phoenix::assets::lower_ascii(rhs);
        });
    }

    void PhoenixRuntime::scan_terrain_textures()
    {
        state_.terrainTextureNames.clear();

        const auto terrainRoot = resolve_ci(state_.dataRoot / "Terrain" / "Detail");
        if (!std::filesystem::exists(terrainRoot))
            return;

        for (const auto& entry : std::filesystem::directory_iterator(terrainRoot))
        {
            if (!entry.is_regular_file())
                continue;

            const auto extension = phoenix::assets::lower_ascii(entry.path().extension().string());
            if (extension != ".dds")
                continue;

            state_.terrainTextureNames.push_back(entry.path().filename().string());
        }

        std::ranges::sort(state_.terrainTextureNames, [](const auto& lhs, const auto& rhs) {
            return phoenix::assets::lower_ascii(lhs) < phoenix::assets::lower_ascii(rhs);
        });
    }

    void PhoenixRuntime::scan_audio_assets()
    {
        state_.audioAssets.clear();
        if (!std::filesystem::exists(state_.dataRoot))
            return;

        std::unordered_set<std::string> seen;
        for (const auto& [relativeKey, path] : state_.assets.byRelativePath)
        {
            if (!is_audio_asset_extension(path.extension().string()))
                continue;

            std::error_code ec;
            const auto relativePath = std::filesystem::relative(path, state_.dataRoot, ec);
            const auto displayName = (!ec && !relativePath.empty())
                ? relativePath.string()
                : path.filename().string();
            const auto uniqueKey = phoenix::assets::lower_ascii(displayName);
            if (!seen.insert(uniqueKey).second)
                continue;

            AudioAsset asset{};
            asset.path = path;
            asset.displayName = displayName;
            asset.fileName = path.filename().string();
            state_.audioAssets.push_back(std::move(asset));
        }

        std::ranges::sort(state_.audioAssets, [](const auto& lhs, const auto& rhs) {
            return phoenix::assets::lower_ascii(lhs.displayName) < phoenix::assets::lower_ascii(rhs.displayName);
        });
    }

    void PhoenixRuntime::update_status()
    {
        if (state_.world.parsed)
        {
            state_.status = std::format(
                "{} | assets {}/{} | objects {}",
                state_.world.path.filename().string(),
                std::ranges::count_if(state_.worldAssets, [](const auto& asset) { return asset.loaded; }),
                state_.worldAssets.size(),
                state_.sceneObjects.size());
        }
        else
        {
            state_.status = std::format(
                "Data files {} | maps {} | no map loaded",
                state_.assets.indexedFiles,
                state_.worldMapPaths.size());
        }
    }

    std::uint32_t PhoenixRuntime::resolve_asset_texture_layer(std::string_view textureName)
    {
        // Memoise by name: the world build asks for the same texture across
        // many meshes, and resolving (3x path lookups + a disk stat) is the bulk of
        // the world-load cost. Same input => same layer, so this is behaviour-exact.
        std::string cacheKey(textureName);
        if (const auto it = assetTextureLayerCache_.find(cacheKey); it != assetTextureLayerCache_.end())
            return it->second;
        const auto cacheResult = [&](std::uint32_t layer) {
            assetTextureLayerCache_.emplace(cacheKey, layer);
            return layer;
        };

        auto path = phoenix::assets::resolve_texture_asset(state_.assets, std::string(textureName));
        if (path.empty() || !std::filesystem::exists(path))
            return cacheResult(0xFFFFFFFFu);

        const auto key = phoenix::assets::lower_ascii(path.string());
        // Universal transparency: cutout is decided by the texture's actual
        // alpha content (pre-warmed in parallel by load_world_assets), never
        // by filename heuristics.
        const auto cutoutIt = textureCutoutCache_.find(key);
        const bool cutout = cutoutIt != textureCutoutCache_.end()
            ? cutoutIt->second
            : textureCutoutCache_.emplace(key, phoenix::renderer::dds_file_has_alpha_cutout(path)).first->second;
        const auto cutoutOffset = cutout ? kAssetCutoutLayerBase : 0u;
        if (const auto it = state_.textureSlotByPath.find(key); it != state_.textureSlotByPath.end())
            return cacheResult(kAssetTextureLayerBase + cutoutOffset + it->second);

        if (state_.assetTexturePaths.size() >= maxAssetTextureLayers_)
            return cacheResult(0xFFFFFFFFu);

        const auto slot = static_cast<std::uint32_t>(state_.assetTexturePaths.size());
        state_.textureSlotByPath.emplace(key, slot);
        state_.assetTexturePaths.push_back(path);
        return cacheResult(kAssetTextureLayerBase + cutoutOffset + slot);
    }

    void PhoenixRuntime::load_world_assets()
    {
        assetTextureLayerCache_.clear();
        state_.worldAssets.clear();
        state_.sceneObjects.clear();
        state_.assetTexturePaths.clear();
        if (!state_.world.parsed)
            return;

        if (state_.world.isDungeon && !state_.world.dungeonDgFileName.empty())
        {
            phoenix::world::WldObjectSection dgSection{};
            dgSection.name = "DungeonMain";
            dgSection.assets.push_back(state_.world.dungeonDgFileName);
            phoenix::world::WldObjectInstance dgInstance{};
            dgInstance.assetIndex = 0;
            dgInstance.position[0] = 0.0f;
            dgInstance.position[1] = 0.0f;
            dgInstance.position[2] = 0.0f;
            dgInstance.rotationForward[2] = 1.0f;
            dgInstance.rotationUp[1] = 1.0f;
            dgSection.instances.push_back(dgInstance);
            state_.world.objectSections.insert(state_.world.objectSections.begin(), std::move(dgSection));
        }

        std::unordered_set<std::string> seen;
        state_.textureSlotByPath.clear();
        const auto assetTextureLayer = [this](std::string_view textureName) -> std::uint32_t {
            return resolve_asset_texture_layer(textureName);
        };

        // ---- Pass 0: collect unique assets in load order (serial dedup). ----
        struct PendingAsset
        {
            std::string name;
            std::string key;
            std::string sectionName;
            std::filesystem::path path;
            int kind{};   // 1 = smod/vani, 2 = dg
            phoenix::world::SmodModel smod;
            phoenix::world::DgModel dg;
        };
        std::vector<PendingAsset> pending;
        for (const auto& section : state_.world.objectSections)
        {
            for (const auto& assetName : section.assets)
            {
                const auto key = phoenix::assets::lower_ascii(assetName);
                if (!seen.insert(key).second)
                    continue;
                PendingAsset p;
                p.name = assetName;
                p.key = key;
                p.sectionName = section.name;

                p.path = state_.assets.resolve(assetName);

                if (key.ends_with(".smod") || key.ends_with(".vani")) p.kind = 1;
                else if (key.ends_with(".dg")) p.kind = 2;
                pending.push_back(std::move(p));
            }
        }

        // ---- Pass 1: parse models from disk in parallel (pure, no shared state). ----
        {
            std::atomic<std::size_t> nextIdx{ 0 };
            const auto workerCount = std::min(
                static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
                std::max<std::size_t>(1, pending.size()));
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t w = 0; w < workerCount; ++w)
            {
                workers.emplace_back([&pending, &nextIdx]() {
                    for (;;)
                    {
                        const auto i = nextIdx.fetch_add(1);
                        if (i >= pending.size()) break;
                        auto& p = pending[i];
                        if (p.path.empty()) continue;
                        if (p.kind == 1)
                            p.smod = p.key.ends_with(".vani")
                                ? phoenix::world::load_vani(p.path)
                                : phoenix::world::load_smod(p.path);
                        else if (p.kind == 2)
                            p.dg = phoenix::world::load_dg(p.path);
                    }
                });
            }
            for (auto& worker : workers) worker.join();
        }

        // ---- Pass 1b: pre-warm the texture cutout cache in parallel. The
        // universal transparency decision inspects each texture's alpha
        // content, which costs a file read — fan it out across cores here so
        // the serial layer-assignment pass below only does map lookups. ----
        {
            std::vector<std::string> uniqueTextures;
            {
                std::unordered_set<std::string> seenTextures;
                for (const auto& p : pending)
                {
                    if (p.kind == 1)
                    {
                        for (const auto& mesh : p.smod.meshes)
                        {
                            if (seenTextures.insert(phoenix::assets::lower_ascii(mesh.textureName)).second)
                                uniqueTextures.push_back(mesh.textureName);
                        }
                    }
                    else if (p.kind == 2)
                    {
                        for (const auto& mesh : p.dg.meshes)
                        {
                            if (seenTextures.insert(phoenix::assets::lower_ascii(mesh.textureName)).second)
                                uniqueTextures.push_back(mesh.textureName);
                        }
                    }
                }
            }

            std::vector<std::pair<std::string, bool>> results(uniqueTextures.size());
            std::atomic<std::size_t> nextIdx{ 0 };
            const auto workerCount = std::min(
                static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
                std::max<std::size_t>(1, uniqueTextures.size()));
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t w = 0; w < workerCount; ++w)
            {
                workers.emplace_back([&]() {
                    for (;;)
                    {
                        const auto i = nextIdx.fetch_add(1);
                        if (i >= uniqueTextures.size()) break;
                        auto path = phoenix::assets::resolve_texture_asset(state_.assets, uniqueTextures[i]);
                        if (path.empty())
                            continue;
                        auto key = phoenix::assets::lower_ascii(path.string());
                        // Already classified on a previous map: skip the file
                        // read entirely (concurrent reads of the cache are safe
                        // — nothing mutates it during this phase).
                        if (textureCutoutCache_.contains(key))
                            continue;
                        results[i].second = phoenix::renderer::dds_file_has_alpha_cutout(path);
                        results[i].first = std::move(key);
                    }
                });
            }
            for (auto& worker : workers) worker.join();

            for (auto& [key, cutout] : results)
            {
                if (!key.empty())
                    textureCutoutCache_.emplace(std::move(key), cutout);
            }
        }

        // ---- Pass 2a: resolve texture layers serially (deterministic slot
        // assignment — same order => identical layer/slot output). ----
        struct MeshLayerInfo
        {
            std::uint32_t materialHash{};
            std::uint32_t textureLayer{};
        };
        std::vector<std::vector<MeshLayerInfo>> meshLayers(pending.size());
        for (std::size_t pi = 0; pi < pending.size(); ++pi)
        {
            const auto& p = pending[pi];
            if (p.path.empty())
                continue;
            if (p.kind == 1)
            {
                meshLayers[pi].reserve(p.smod.meshes.size());
                for (const auto& mesh : p.smod.meshes)
                    meshLayers[pi].push_back({ color_hash(mesh.textureName), assetTextureLayer(mesh.textureName) });
            }
            else if (p.kind == 2)
            {
                meshLayers[pi].reserve(p.dg.meshes.size());
                for (const auto& mesh : p.dg.meshes)
                    meshLayers[pi].push_back({ color_hash(mesh.textureName), assetTextureLayer(mesh.textureName) });
            }
        }

        // ---- Pass 2b: convert vertices/collision per asset in parallel.
        // Each slot is independent; layer/material data comes from pass 2a. ----
        state_.worldAssets.resize(pending.size());
        {
            std::atomic<std::size_t> nextIdx{ 0 };
            const auto workerCount = std::min(
                static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency())),
                std::max<std::size_t>(1, pending.size()));
            const auto buildAsset = [&](std::size_t pi) {
                auto& p = pending[pi];
                auto& asset = state_.worldAssets[pi];
                asset.name = p.name;
                asset.path = p.path;
                const bool isVani = p.key.ends_with(".vani");
                if (!asset.path.empty())
                {
                    if (p.kind == 1)
                    {
                        auto& model = p.smod;
                        asset.loaded = model.parsed;
                        asset.radius = std::max(8.0f, model.radius);
                        asset.vertexAnimated = model.vertexAnimated;
                        asset.frameCount = std::max(1u, model.frameCount);
                        if (model.hasCollision)
                        {
                            asset.hasCollision = true;
                            asset.collisionVertices = std::move(model.collision.vertices);
                            asset.collisionIndices = std::move(model.collision.indices);
                        }
                        for (std::size_t mi = 0; mi < model.meshes.size(); ++mi)
                        {
                            const auto& mesh = model.meshes[mi];
                            // A blank texture name is the original data's own way of
                            // marking a sub-mesh as non-visual (helper/proxy geometry
                            // bundled into the same .smod as the real decor — the
                            // client never renders these). Skip it entirely instead
                            // of drawing it with the "no texture" vertex-color-only
                            // fallback, which otherwise shows up as random floating
                            // solid-colour shapes.
                            if (mesh.textureName.empty())
                                continue;
                            asset.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
                            const auto materialHash = meshLayers[pi][mi].materialHash;
                            const auto textureLayer = meshLayers[pi][mi].textureLayer;
                            const auto base = static_cast<std::uint32_t>(asset.previewVertices.size());
                            asset.previewVertices.reserve(asset.previewVertices.size() + mesh.vertices.size());
                            for (const auto& vertex : mesh.vertices)
                                append_preview_vertex(asset.previewVertices, vertex.position, vertex.normal, vertex.uv, materialHash, textureLayer);
                            asset.previewIndices.reserve(asset.previewIndices.size() + mesh.faces.size() * 3u);
                            for (const auto& face : mesh.faces)
                            {
                                asset.previewIndices.push_back(base + face.indices[0]);
                                asset.previewIndices.push_back(base + face.indices[1]);
                                asset.previewIndices.push_back(base + face.indices[2]);
                            }

                            if (asset.vertexAnimated && mesh.animationFrames.size() == asset.frameCount)
                            {
                                if (!asset.animationFrames)
                                    asset.animationFrames = std::make_shared<std::vector<std::vector<phoenix::renderer::TerrainVertex>>>(asset.frameCount);
                                for (std::uint32_t frame = 0; frame < asset.frameCount; ++frame)
                                {
                                    auto& frameVertices = (*asset.animationFrames)[frame];
                                    frameVertices.reserve(frameVertices.size() + mesh.animationFrames[frame].size());
                                    for (const auto& vertex : mesh.animationFrames[frame])
                                        append_preview_vertex(frameVertices, vertex.position, vertex.normal, vertex.uv, materialHash, textureLayer);
                                }
                            }
                        }
                    }
                    else if (p.kind == 2)
                    {
                        auto& model = p.dg;
                        asset.loaded = model.parsed;
                        asset.radius = std::max({ 12.0f, model.extent[0], model.extent[1], model.extent[2] });
                        if (model.hasCollision)
                        {
                            asset.hasCollision = true;
                            asset.collisionVertices = std::move(model.collision.vertices);
                            asset.collisionIndices = std::move(model.collision.indices);
                        }
                        for (std::size_t mi = 0; mi < model.meshes.size(); ++mi)
                        {
                            const auto& mesh = model.meshes[mi];
                            // See the matching skip in the .smod branch above: a
                            // blank texture name marks non-visual helper geometry.
                            if (mesh.textureName.empty())
                                continue;
                            asset.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
                            const auto materialHash = meshLayers[pi][mi].materialHash;
                            const auto textureLayer = meshLayers[pi][mi].textureLayer;
                            const auto base = static_cast<std::uint32_t>(asset.previewVertices.size());
                            // Lightmap pages only apply when the whole map IS a
                            // dungeon. A DG placed as an open-world asset must
                            // not sample the field lightmap array with its own
                            // page indices.
                            const bool meshHasLightmap = state_.world.isDungeon
                                && model.lightmapCount > 0
                                && mesh.lightmapIndex >= 0
                                && static_cast<std::uint32_t>(mesh.lightmapIndex) < model.lightmapCount;
                            asset.previewVertices.reserve(asset.previewVertices.size() + mesh.vertices.size());
                            for (const auto& vertex : mesh.vertices)
                            {
                                if (meshHasLightmap)
                                    append_preview_vertex_lightmapped(asset.previewVertices, vertex.position,
                                        vertex.normal, vertex.uv, vertex.lightmapUv, mesh.lightmapIndex, textureLayer);
                                else
                                    append_preview_vertex(asset.previewVertices, vertex.position, vertex.normal,
                                        vertex.uv, materialHash, textureLayer);
                            }
                            asset.previewIndices.reserve(asset.previewIndices.size() + mesh.indices.size());
                            for (const auto index : mesh.indices)
                                asset.previewIndices.push_back(base + index);
                        }
                    }
                }

                // The decoded SMOD/VANI/DG vertices are still in Shaiya's
                // left-handed local space.  Reflect local X as well as the
                // instance origin/basis: this is the same two-boundary
                // conversion used by the Godot client and keeps every asset
                // on its authored side of its pivot.  Reflection changes the
                // triangle determinant, so preserve front-face winding.
                for (auto& vertex : asset.previewVertices)
                {
                    vertex.position[0] = -vertex.position[0];
                    vertex.normal[0] = -vertex.normal[0];
                }
                for (std::size_t i = 0; i + 2 < asset.previewIndices.size(); i += 3)
                    std::swap(asset.previewIndices[i + 1], asset.previewIndices[i + 2]);

                if (asset.animationFrames)
                {
                    for (auto& frame : *asset.animationFrames)
                    {
                        for (auto& vertex : frame)
                        {
                            vertex.position[0] = -vertex.position[0];
                            vertex.normal[0] = -vertex.normal[0];
                        }
                    }
                }

                for (std::size_t i = 0; i + 2 < asset.collisionVertices.size(); i += 3)
                    asset.collisionVertices[i] = -asset.collisionVertices[i];
                for (std::size_t i = 0; i + 2 < asset.collisionIndices.size(); i += 3)
                    std::swap(asset.collisionIndices[i + 1], asset.collisionIndices[i + 2]);

                // Fallback: generate collision from visual mesh for assets without
                // explicit collision data. Uses the preview mesh positions directly.
                // Skip Grass section - these should never block movement.
                if (!asset.hasCollision && asset.loaded && !isVani
                    && !asset.previewVertices.empty() && !asset.previewIndices.empty()
                    && !asset.vertexAnimated && p.sectionName != "Grass")
                {
                    asset.collisionVertices.resize(asset.previewVertices.size() * 3);
                    for (std::size_t vi = 0; vi < asset.previewVertices.size(); ++vi)
                    {
                        asset.collisionVertices[vi * 3 + 0] = asset.previewVertices[vi].position[0];
                        asset.collisionVertices[vi * 3 + 1] = asset.previewVertices[vi].position[1];
                        asset.collisionVertices[vi * 3 + 2] = asset.previewVertices[vi].position[2];
                    }
                    asset.collisionIndices = asset.previewIndices;
                    asset.hasCollision = true;
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (std::size_t w = 0; w < workerCount; ++w)
            {
                workers.emplace_back([&pending, &nextIdx, &buildAsset]() {
                    for (;;)
                    {
                        const auto i = nextIdx.fetch_add(1);
                        if (i >= pending.size()) break;
                        buildAsset(i);
                    }
                });
            }
            for (auto& worker : workers) worker.join();
        }
        std::unordered_map<std::string, const LoadedWorldAsset*> assetByName;
        std::unordered_map<std::string, std::int32_t> assetSlotByName;
        for (std::size_t index = 0; index < state_.worldAssets.size(); ++index)
        {
            const auto key = phoenix::assets::lower_ascii(state_.worldAssets[index].name);
            assetByName.emplace(key, &state_.worldAssets[index]);
            assetSlotByName.emplace(key, static_cast<std::int32_t>(index));
        }

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto halfMap = state_.world.isDungeon ? 0.0f : mapSize * 0.5f;
        state_.sceneObjects.reserve(70000);

        std::unordered_map<int, phoenix::world::ManiAnimation> maniCache;
        const auto maniAnimationFor = [&](std::int32_t maniAssetIndex) -> const phoenix::world::ManiAnimation& {
            static const phoenix::world::ManiAnimation empty{};
            if (maniAssetIndex < 0
                || static_cast<std::size_t>(maniAssetIndex) >= state_.world.maniAssets.size())
                return empty;

            if (const auto it = maniCache.find(maniAssetIndex); it != maniCache.end())
                return it->second;

            auto animation = phoenix::world::ManiAnimation{};
            const auto& maniName = state_.world.maniAssets[static_cast<std::size_t>(maniAssetIndex)];
            const auto maniPath = state_.assets.resolve(maniName);
            if (!maniPath.empty())
                animation = phoenix::world::load_mani(maniPath);

            const auto [it, inserted] = maniCache.emplace(maniAssetIndex, animation);
            (void)inserted;
            return it->second;
        };

        for (std::size_t sectionIdx = 0; sectionIdx < state_.world.objectSections.size(); ++sectionIdx)
        {
            const auto& section = state_.world.objectSections[sectionIdx];
            for (std::size_t instIdx = 0; instIdx < section.instances.size(); ++instIdx)
            {
                const auto& instance = section.instances[instIdx];
                SceneObject object{};
                object.x = phoenix::world::source_to_world_x(instance.position[0], halfMap);
                object.y = instance.position[1];
                object.z = phoenix::world::source_to_world_z(instance.position[2], halfMap);
                std::copy(std::begin(instance.rotationForward), std::end(instance.rotationForward), std::begin(object.forward));
                std::copy(std::begin(instance.rotationUp), std::end(instance.rotationUp), std::begin(object.up));
                phoenix::world::mirror_source_direction_x(object.forward);
                phoenix::world::mirror_source_direction_x(object.up);
                object.sectionIndex = static_cast<std::int32_t>(sectionIdx);
                object.instanceIndex = static_cast<std::int32_t>(instIdx);

                const auto assetIndex = instance.assetIndex >= 0
                    ? static_cast<std::size_t>(instance.assetIndex)
                    : std::numeric_limits<std::size_t>::max();
                if (assetIndex < section.assets.size())
                {
                    const auto key = phoenix::assets::lower_ascii(section.assets[assetIndex]);
                    if (const auto it = assetByName.find(key); it != assetByName.end())
                    {
                        object.loaded = it->second->loaded;
                        object.radius = it->second->radius;
                    }
                    if (const auto slot = assetSlotByName.find(key); slot != assetSlotByName.end())
                        object.assetSlot = slot->second;
                }
                state_.sceneObjects.push_back(object);
            }
        }

        // ---- MANI instances: separate placements of Building assets ----
        // Each WldManiInstance places a Building asset at its own position/rotation.
        // buildingAssetId indexes into the Building section's asset list.
        if (!state_.world.maniInstances.empty())
        {
            // Find the Building section to resolve asset names.
            std::int32_t buildingSectionIdx = -1;
            for (std::size_t s = 0; s < state_.world.objectSections.size(); ++s)
            {
                if (state_.world.objectSections[s].name == "Building")
                {
                    buildingSectionIdx = static_cast<std::int32_t>(s);
                    break;
                }
            }

            if (buildingSectionIdx >= 0)
            {
                const auto& buildingSection = state_.world.objectSections[static_cast<std::size_t>(buildingSectionIdx)];
                for (std::size_t mi = 0; mi < state_.world.maniInstances.size(); ++mi)
                {
                    const auto& maniInst = state_.world.maniInstances[mi];
                    const auto bId = maniInst.buildingAssetId;
                    if (bId < 0 || static_cast<std::size_t>(bId) >= buildingSection.assets.size())
                        continue;

                    const auto key = phoenix::assets::lower_ascii(buildingSection.assets[static_cast<std::size_t>(bId)]);

                    SceneObject object{};
                    object.x = phoenix::world::source_to_world_x(maniInst.position[0], halfMap);
                    object.y = maniInst.position[1];
                    object.z = phoenix::world::source_to_world_z(maniInst.position[2], halfMap);
                    std::copy(std::begin(maniInst.rotationForward), std::end(maniInst.rotationForward), std::begin(object.forward));
                    std::copy(std::begin(maniInst.rotationUp), std::end(maniInst.rotationUp), std::begin(object.up));
                    phoenix::world::mirror_source_direction_x(object.forward);
                    phoenix::world::mirror_source_direction_x(object.up);
                    object.sectionIndex = buildingSectionIdx;
                    object.instanceIndex = -1; // not a regular section instance

                    if (const auto it = assetByName.find(key); it != assetByName.end())
                    {
                        object.loaded = it->second->loaded;
                        object.radius = it->second->radius;
                    }
                    if (const auto slot = assetSlotByName.find(key); slot != assetSlotByName.end())
                        object.assetSlot = slot->second;

                    const auto& mani = maniAnimationFor(maniInst.maniAssetIndex);
                    if (mani.parsed && mani.enableRotation && std::abs(mani.animationSpeed) >= 0.0001f)
                    {
                        std::copy(std::begin(mani.rotationAxis), std::end(mani.rotationAxis), std::begin(object.maniRotationAxis));
                        phoenix::world::mirror_source_direction_x(object.maniRotationAxis);
                        object.maniRotationSpeed = mani.animationSpeed * kManiTicksPerSecond;
                    }

                    state_.sceneObjects.push_back(object);
                }
            }
        }

    }

    PhoenixRuntime::LoadedEffectLibrary PhoenixRuntime::load_effect_library_file(const std::filesystem::path& path) const
    {
        LoadedEffectLibrary result{};
        result.library = phoenix::world::load_eft(path);
        if (!result.library.parsed)
            return result;

        result.texturePaths.reserve(result.library.textureNames.size());
        for (const auto& textureName : result.library.textureNames)
            result.texturePaths.push_back(textureName.empty() ? std::filesystem::path{} : state_.assets.resolve(textureName));

        result.meshes.reserve(result.library.meshNames.size());
        for (const auto& meshName : result.library.meshNames)
        {
            if (meshName.empty())
            {
                result.meshes.push_back({});
                continue;
            }
            const auto meshPath = state_.assets.resolve(meshName);
            result.meshes.push_back(
                meshPath.empty() ? phoenix::world::EftMesh{} : phoenix::world::load_eft_mesh(meshPath));
        }
        return result;
    }

    std::vector<std::filesystem::path> PhoenixRuntime::effect_library_files() const
    {
        std::vector<std::filesystem::path> files;
        const auto effectsRoot = resolve_ci(state_.dataRoot / "effects");
        if (effectsRoot.empty() || !std::filesystem::is_directory(effectsRoot))
            return files;

        for (const auto& entry : std::filesystem::directory_iterator(effectsRoot))
        {
            if (!entry.is_regular_file())
                continue;
            const auto ext = phoenix::assets::lower_ascii(entry.path().extension().string());
            if (ext == ".eft" || ext == ".ef2" || ext == ".ef3")
                files.push_back(entry.path());
        }
        std::ranges::sort(files, {}, [](const std::filesystem::path& p) {
            return phoenix::assets::lower_ascii(p.filename().string());
        });
        return files;
    }

    void PhoenixRuntime::load_effect_library()
    {
        state_.effectLibrary = {};
        state_.effectTextureLayers.clear();
        state_.effectMeshes.clear();
        state_.effectPlacements.clear();

        if (state_.world.effectFileName.empty())
            return;

        const auto eftPath = state_.assets.resolve(state_.world.effectFileName);
        if (eftPath.empty())
            return;

        auto loaded = load_effect_library_file(eftPath);
        if (!loaded.library.parsed)
            return;
        state_.effectLibrary = std::move(loaded.library);
        state_.effectMeshes = std::move(loaded.meshes);

        // The map's own linked effect file goes into the shared asset texture
        // array like everything else (unlike an arbitrary file browsed from
        // the debug panel — see LoadedEffectLibrary's doc comment).
        state_.effectTextureLayers.reserve(state_.effectLibrary.textureNames.size());
        for (const auto& textureName : state_.effectLibrary.textureNames)
        {
            if (textureName.empty())
            {
                state_.effectTextureLayers.push_back(0xFFFFFFFFu);
                continue;
            }
            state_.effectTextureLayers.push_back(resolve_asset_texture_layer(textureName));
        }

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto halfMap = state_.world.isDungeon ? 0.0f : mapSize * 0.5f;

        const auto normalize = [](float* vector, const float* fallback) {
            const auto length = std::sqrt(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
            if (length < 0.001f)
            {
                vector[0] = fallback[0];
                vector[1] = fallback[1];
                vector[2] = fallback[2];
                return;
            }
            vector[0] /= length;
            vector[1] /= length;
            vector[2] /= length;
        };

        state_.effectPlacements.reserve(state_.world.effectInstances.size());
        for (const auto& inst : state_.world.effectInstances)
        {
            if (inst.effectId < 0
                || static_cast<std::size_t>(inst.effectId) >= state_.effectLibrary.sequences.size())
                continue;

            EffectPlacement placement{};
            placement.position[0] = phoenix::world::source_to_world_x(inst.position[0], halfMap);
            placement.position[1] = inst.position[1];
            placement.position[2] = phoenix::world::source_to_world_z(inst.position[2], halfMap);

            float forward[3]{ inst.rotationForward[0], inst.rotationForward[1], inst.rotationForward[2] };
            float up[3]{ inst.rotationUp[0], inst.rotationUp[1], inst.rotationUp[2] };
            const float fallbackForward[3]{ 0.0f, 0.0f, 1.0f };
            const float fallbackUp[3]{ 0.0f, 1.0f, 0.0f };
            normalize(forward, fallbackForward);
            normalize(up, fallbackUp);
            float right[3]{
                up[1] * forward[2] - up[2] * forward[1],
                up[2] * forward[0] - up[0] * forward[2],
                up[0] * forward[1] - up[1] * forward[0],
            };
            // Effect geometry remains in its authored local space, unlike
            // SMOD/DG geometry.  Reflect every source basis column directly;
            // recomputing right after reflection would introduce the extra
            // determinant sign of a cross product and flip the effect again.
            phoenix::world::mirror_source_direction_x(forward);
            phoenix::world::mirror_source_direction_x(up);
            phoenix::world::mirror_source_direction_x(right);
            std::copy(std::begin(forward), std::end(forward), std::begin(placement.forward));
            std::copy(std::begin(up), std::end(up), std::begin(placement.up));
            std::copy(std::begin(right), std::end(right), std::begin(placement.right));
            placement.sequenceIndex = inst.effectId;

            state_.effectPlacements.push_back(placement);
        }
    }

    float PhoenixRuntime::terrain_height(float worldX, float worldZ) const
    {
        if (!state_.world.parsed || state_.world.heightSamples.empty() || state_.world.heightMapSide < 2)
            return 0.0f;

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto u = phoenix::world::world_to_source_u(worldX, mapSize);
        const auto v = phoenix::world::world_to_source_v(worldZ, mapSize);
        const auto side = state_.world.heightMapSide;
        const auto fx = u * static_cast<float>(side - 1);
        const auto fz = v * static_cast<float>(side - 1);
        const auto x0 = static_cast<std::uint32_t>(std::floor(fx));
        const auto z0 = static_cast<std::uint32_t>(std::floor(fz));
        const auto x1 = std::min(side - 1, x0 + 1);
        const auto z1 = std::min(side - 1, z0 + 1);
        const auto tx = fx - static_cast<float>(x0);
        const auto tz = fz - static_cast<float>(z0);
        const auto sample = [&](std::uint32_t x, std::uint32_t z) {
            return (state_.world.heightSamples[static_cast<std::size_t>(z) * side + x] - 10000.0f) / 50.0f;
        };
        const auto a = std::lerp(sample(x0, z0), sample(x1, z0), tx);
        const auto b = std::lerp(sample(x0, z1), sample(x1, z1), tx);
        return std::lerp(a, b, tz);
    }

    std::filesystem::path PhoenixRuntime::walk_sound_at(float worldX, float worldZ) const
    {
        const auto& w = state_.world;
        if (w.terrainTextureMap.empty() || w.heightMapSide < 2 || w.terrainLayers.empty())
            return {};

        const auto mapSize = static_cast<float>(std::max(1u, w.mapSize));
        const auto u = phoenix::world::world_to_source_u(worldX, mapSize);
        const auto v = phoenix::world::world_to_source_v(worldZ, mapSize);
        const auto side = w.heightMapSide;
        const auto ix = std::min(static_cast<std::uint32_t>(u * static_cast<float>(side - 1)), side - 1);
        const auto iz = std::min(static_cast<std::uint32_t>(v * static_cast<float>(side - 1)), side - 1);
        const auto idx = static_cast<std::size_t>(iz) * side + ix;
        if (idx >= w.terrainTextureMap.size())
            return {};

        const auto layerIndex = static_cast<std::size_t>(w.terrainTextureMap[idx]);
        if (layerIndex >= w.terrainLayers.size())
            return {};

        const auto& soundName = w.terrainLayers[layerIndex].walkSoundFileName;
        if (soundName.empty())
            return {};

        return audio_path_for(soundName);
    }

    PreviewImage PhoenixRuntime::create_3d_preview_image(std::uint32_t width, std::uint32_t height) const
    {
        PreviewImage image{};
        image.width = std::max(1u, width);
        image.height = std::max(1u, height);
        image.bgra.assign(static_cast<std::size_t>(image.width) * image.height * 4, 255);

        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            const auto t = static_cast<float>(y) / static_cast<float>(std::max(1u, image.height - 1));
            const auto sky = t < 0.55f;
            const auto r = sky ? static_cast<std::uint8_t>(72 - t * 30.0f) : static_cast<std::uint8_t>(24);
            const auto g = sky ? static_cast<std::uint8_t>(104 - t * 35.0f) : static_cast<std::uint8_t>(28);
            const auto b = sky ? static_cast<std::uint8_t>(135 - t * 28.0f) : static_cast<std::uint8_t>(30);
            for (std::uint32_t x = 0; x < image.width; ++x)
                put_pixel(image, static_cast<int>(x), static_cast<int>(y), r, g, b);
        }

        if (!state_.world.parsed || state_.world.heightSamples.empty())
            return image;

        const auto project = [&](float wx, float wy, float wz) -> std::optional<ProjectedPoint> {
            const auto dx = wx - camera_.x;
            const auto dy = wy - camera_.y;
            const auto dz = wz - camera_.z;
            const auto cy = std::cos(camera_.yaw);
            const auto sy = std::sin(camera_.yaw);
            const auto cp = std::cos(camera_.pitch);
            const auto sp = std::sin(camera_.pitch);

            const auto cameraX = cy * dx - sy * dz;
            const auto yawZ = sy * dx + cy * dz;
            const auto cameraY = cp * dy - sp * yawZ;
            const auto cameraZ = sp * dy + cp * yawZ;
            if (cameraZ < 8.0f)
                return std::nullopt;

            const auto focal = static_cast<float>(image.height) * 0.86f;
            ProjectedPoint p{};
            p.x = static_cast<int>(static_cast<float>(image.width) * 0.5f + cameraX * focal / cameraZ);
            p.y = static_cast<int>(static_cast<float>(image.height) * 0.52f - cameraY * focal / cameraZ);
            p.depth = cameraZ;
            if (p.x < -400 || p.x > static_cast<int>(image.width) + 400 || p.y < -400 || p.y > static_cast<int>(image.height) + 400)
                return std::nullopt;
            return p;
        };

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto halfMap = mapSize * 0.5f;
        const auto grid = 56u;
        const auto step = mapSize / static_cast<float>(grid);
        std::vector<std::optional<ProjectedPoint>> projected((grid + 1) * (grid + 1));

        for (std::uint32_t z = 0; z <= grid; ++z)
        {
            for (std::uint32_t x = 0; x <= grid; ++x)
            {
                const auto wx = -halfMap + static_cast<float>(x) * step;
                const auto wz = -halfMap + static_cast<float>(z) * step;
                const auto wy = terrain_height(wx, wz);
                projected[static_cast<std::size_t>(z) * (grid + 1) + x] = project(wx, wy, wz);
            }
        }

        for (std::uint32_t z = 0; z <= grid; ++z)
        {
            for (std::uint32_t x = 0; x <= grid; ++x)
            {
                const auto current = projected[static_cast<std::size_t>(z) * (grid + 1) + x];
                if (!current)
                    continue;

                const auto wx = -halfMap + static_cast<float>(x) * step;
                const auto wz = -halfMap + static_cast<float>(z) * step;
                const auto h = terrain_height(wx, wz);
                const auto water = h < 1.5f;
                const auto shade = static_cast<std::uint8_t>(std::clamp((h + 35.0f) / 155.0f, 0.0f, 1.0f) * 80.0f);
                const auto r = water ? static_cast<std::uint8_t>(31) : static_cast<std::uint8_t>(48 + shade);
                const auto g = water ? static_cast<std::uint8_t>(102 + shade / 3) : static_cast<std::uint8_t>(104 + shade);
                const auto b = water ? static_cast<std::uint8_t>(133 + shade / 2) : static_cast<std::uint8_t>(45 + shade / 3);

                if (x + 1 <= grid)
                {
                    const auto right = projected[static_cast<std::size_t>(z) * (grid + 1) + x + 1];
                    if (right)
                        draw_line(image, current->x, current->y, right->x, right->y, r, g, b);
                }
                if (z + 1 <= grid)
                {
                    const auto down = projected[static_cast<std::size_t>(z + 1) * (grid + 1) + x];
                    if (down)
                        draw_line(image, current->x, current->y, down->x, down->y, r, g, b);
                }
            }
        }

        struct ObjectDraw
        {
            float depth{};
            int x{};
            int y{};
            int radius{};
            bool loaded{};
        };
        std::vector<ObjectDraw> objects;
        objects.reserve(2048);
        constexpr std::size_t kMaxVisibleObjectMarkers = 3500;
        constexpr float kObjectViewDistance = 950.0f;

        for (const auto& sceneObject : state_.sceneObjects)
        {
            if (objects.size() >= kMaxVisibleObjectMarkers)
                break;

            const auto dx = sceneObject.x - camera_.x;
            const auto dz = sceneObject.z - camera_.z;
            if (dx * dx + dz * dz > kObjectViewDistance * kObjectViewDistance)
                continue;

            const auto wy = sceneObject.y + std::max(6.0f, sceneObject.radius * 0.30f);
            if (const auto p = project(sceneObject.x, wy, sceneObject.z))
            {
                const auto screenRadius = static_cast<int>(std::clamp(sceneObject.radius * 220.0f / p->depth, 2.0f, 9.0f));
                objects.push_back({ p->depth, p->x, p->y, screenRadius, sceneObject.loaded });
            }
        }

        std::ranges::sort(objects, [](const auto& lhs, const auto& rhs) {
            return lhs.depth > rhs.depth;
        });

        for (const auto& object : objects)
        {
            if (object.loaded)
            {
                draw_dot(image, object.x, object.y, object.radius + 1, 58, 36, 12);
                draw_dot(image, object.x, object.y, object.radius, 241, 177, 44);
            }
            else
            {
                draw_dot(image, object.x, object.y, object.radius + 1, 72, 25, 25);
                draw_dot(image, object.x, object.y, object.radius, 220, 72, 72);
            }
        }

        return image;
    }

    std::vector<std::filesystem::path> PhoenixRuntime::terrain_texture_paths() const
    {
        return terrain_detail_paths_for_map(state_.assets, state_.world);
    }

    std::vector<std::filesystem::path> PhoenixRuntime::field_lightmap_paths(std::uint32_t& sectionCount) const
    {
        sectionCount = 0;
        if (!state_.world.parsed || state_.world.isDungeon)
            return {};

        // Derive map stem from the WLD path filename (e.g., "1" from "1.wld").
        const auto stem = state_.world.path.stem().string();
        if (stem.empty())
            return {};

        // Use the world's own field/ folder, or fall back to Data/World/field/<mapId>/.
        auto fieldDir = state_.world.phoenixWorldFieldDir;
        if (fieldDir.empty())
            fieldDir = resolve_ci(state_.dataRoot / "World" / "field" / stem);
        if (fieldDir.empty() || !std::filesystem::is_directory(fieldDir))
            return {};

        // Big maps (mapSize >= 1536): 2x2 sections (00,01,10,11).
        // Small maps: 1x1 section (00 only).
        const bool bigMap = state_.world.mapSize >= 1536;
        sectionCount = bigMap ? 2 : 1;

        std::vector<std::filesystem::path> paths;
        const std::string sections[] = { "00", "01", "10", "11" };
        const auto total = bigMap ? 4u : 1u;
        for (std::uint32_t i = 0; i < total; ++i)
        {
            const auto name = stem + "_" + sections[i] + "_l.dds";
            auto p = resolve_ci(fieldDir / name);
            paths.push_back(std::move(p));
        }
        return paths;
    }

    std::vector<std::filesystem::path> PhoenixRuntime::field_alpha_mask_paths(std::uint32_t& layerFlags) const
    {
        layerFlags = 0;
        if (!state_.world.parsed || state_.world.isDungeon)
            return {};

        const auto stem = state_.world.path.stem().string();
        if (stem.empty())
            return {};

        auto fieldDir = state_.world.phoenixWorldFieldDir;
        if (fieldDir.empty())
            fieldDir = resolve_ci(state_.dataRoot / "World" / "field" / stem);
        if (fieldDir.empty() || !std::filesystem::is_directory(fieldDir))
            return {};

        const bool bigMap = state_.world.mapSize >= 1536;
        const std::string sections[] = { "00", "01", "10", "11" };
        const auto sectionTotal = bigMap ? 4u : 1u;

        // Ordered section-major: [section][maskLayer 0..7].
        std::vector<std::filesystem::path> paths;
        paths.reserve(static_cast<std::size_t>(sectionTotal) * kFieldAlphaMaskLayers);
        for (std::uint32_t s = 0; s < sectionTotal; ++s)
        {
            for (std::uint32_t n = 0; n < kFieldAlphaMaskLayers; ++n)
            {
                const auto name = stem + "_" + sections[s] + "_a" + std::to_string(n) + ".dds";
                auto p = resolve_ci(fieldDir / name);
                if (!p.empty() && std::filesystem::exists(p))
                    layerFlags |= 1u << n;
                else
                    p.clear();
                paths.push_back(std::move(p));
            }
        }
        if (layerFlags == 0)
            return {};
        return paths;
    }

    std::vector<std::filesystem::path> PhoenixRuntime::dungeon_lightmap_paths() const
    {
        if (!state_.world.parsed || !state_.world.isDungeon
            || state_.world.dungeonDgFileName.empty())
            return {};

        const auto dgPath = state_.assets.resolve(state_.world.dungeonDgFileName);
        if (dgPath.empty())
            return {};

        // Pages live in a folder named after the DG next to it:
        // dungeon/<name>.dg + dungeon/<name>/<name>_L<i>.dds
        const auto stem = dgPath.stem().string();
        auto pageDir = resolve_ci(dgPath.parent_path() / stem);
        if (pageDir.empty() || !std::filesystem::is_directory(pageDir))
            pageDir = dgPath.parent_path(); // fallback: pages next to the DG

        std::vector<std::filesystem::path> paths;
        for (std::uint32_t i = 0; i < 256; ++i)
        {
            auto p = resolve_ci(pageDir / (stem + "_L" + std::to_string(i) + ".dds"));
            if (p.empty() || !std::filesystem::exists(p))
                break;
            paths.push_back(std::move(p));
        }
        return paths;
    }

    const std::vector<std::filesystem::path>& PhoenixRuntime::asset_texture_paths() const
    {
        return state_.assetTexturePaths;
    }

    std::filesystem::path PhoenixRuntime::texture_path_for(std::string_view fileName) const
    {
        if (fileName.empty())
            return {};

        auto path = phoenix::assets::resolve_texture_asset(state_.assets, std::string(fileName));
        if (!path.empty() && std::filesystem::exists(path))
            return path;

        const auto skyPath = resolve_ci(state_.dataRoot / "Sky" / fileName);
        if (std::filesystem::exists(skyPath))
            return skyPath;

        return {};
    }

    std::filesystem::path PhoenixRuntime::audio_path_for(std::string_view fileName) const
    {
        if (fileName.empty())
            return {};

        const auto requested = std::filesystem::path(std::string(fileName));
        const auto parentDir = requested.parent_path();
        const auto stem = requested.filename().stem().string();
        const auto oggName = stem + ".ogg";

        const std::filesystem::path roots[] = {
            resolve_ci(state_.dataRoot / "Sound"),
            resolve_ci(state_.dataRoot / "Sounds"),
            resolve_ci(state_.dataRoot / "Music"),
            resolve_ci(state_.dataRoot / "BGM"),
            resolve_ci(state_.dataRoot / "Audio"),
        };

        auto path = state_.assets.resolve(oggName);
        if (!path.empty() && std::filesystem::exists(path))
            return path;

        if (!parentDir.empty())
        {
            const auto relativeOgg = parentDir / oggName;
            path = state_.assets.resolve(relativeOgg.string());
            if (!path.empty() && std::filesystem::exists(path))
                return path;
        }

        for (const auto& root : roots)
        {
            if (!parentDir.empty())
            {
                path = root / parentDir / oggName;
                if (std::filesystem::exists(path))
                    return path;
            }
            path = root / oggName;
            if (std::filesystem::exists(path))
                return path;
        }

        return {};
    }

    std::filesystem::path PhoenixRuntime::water_texture_path() const
    {
        if (!state_.world.layoutName.empty())
        {
            auto path = phoenix::assets::resolve_texture_asset(state_.assets, state_.world.layoutName);
            if (!path.empty() && std::filesystem::exists(path))
                return path;
        }

        const char* candidates[] = {
            "D_Water01.dds",
            "water_t.dds",
            "water001.dds",
            "water0001.dds",
            "B8_water.DDS",
        };

        for (const auto* candidate : candidates)
        {
            auto path = state_.assets.resolve(candidate);
            if (!path.empty())
                return path;
        }

        return {};
    }

    bool PhoenixRuntime::load_water_animation()
    {
        state_.waterAnimation = {};
        if (state_.entityRoot.empty())
            return false;

        // Find a WTR file in Entity/Water.
        const auto waterDir = resolve_ci(state_.entityRoot / "Water");
        if (!std::filesystem::exists(waterDir))
            return false;

        // Prefer "World.wtr", then any .wtr.
        std::filesystem::path wtrPath;
        const char* preferred[] = { "World.wtr", "B1_Water.wtr", "B8.wtr", "A2.wtr" };
        for (const auto* name : preferred)
        {
            auto candidate = waterDir / name;
            if (std::filesystem::exists(candidate))
            {
                wtrPath = candidate;
                break;
            }
        }
        if (wtrPath.empty())
        {
            for (const auto& entry : std::filesystem::directory_iterator(waterDir))
            {
                auto ext = phoenix::assets::lower_ascii(entry.path().extension().string());
                if (ext == ".wtr") { wtrPath = entry.path(); break; }
            }
        }
        if (wtrPath.empty())
            return false;

        // Parse WTR: 16-byte header + N entries of 256 bytes (filename string padded).
        auto data = assets::read_file_binary(wtrPath);
        if (data.size() < 16) return false;

        float tileSize{};
        std::memcpy(&tileSize, data.data(), 4);
        std::uint32_t frameCount{};
        std::memcpy(&frameCount, data.data() + 12, 4);

        if (frameCount == 0 || frameCount > 256)
            return false;

        state_.waterAnimation.tileSize = std::max(1.0f, tileSize);
        state_.waterAnimation.frameCount = frameCount;

        const std::size_t entrySize = 256;
        for (std::uint32_t i = 0; i < frameCount; ++i)
        {
            const auto offset = 16 + static_cast<std::size_t>(i) * entrySize;
            if (offset + entrySize > data.size())
                break;

            // Read null-terminated filename.
            std::string name;
            for (std::size_t j = offset; j < offset + entrySize && data[j] != 0; ++j)
                name.push_back(static_cast<char>(data[j]));

            // WTR references .jpg/.tga but actual files are .dds on disk.
            auto stem = std::filesystem::path(name).stem().string();
            auto ddsName = stem + ".dds";

            // Try to find the DDS file.
            auto ddsPath = waterDir / ddsName;
            if (std::filesystem::exists(ddsPath))
            {
                state_.waterAnimation.frameFileNames.push_back(ddsName);
                state_.waterAnimation.framePaths.push_back(ddsPath);
            }
            else
            {
                // Try resolving through asset index.
                auto resolved = state_.assets.resolve(ddsName);
                if (!resolved.empty() && std::filesystem::exists(resolved))
                {
                    state_.waterAnimation.frameFileNames.push_back(ddsName);
                    state_.waterAnimation.framePaths.push_back(resolved);
                }
            }
        }

        // Remove duplicate consecutive frames (WTR can reference same frame multiple times).
        std::vector<std::filesystem::path> uniquePaths;
        std::vector<std::string> uniqueNames;
        for (std::size_t i = 0; i < state_.waterAnimation.framePaths.size(); ++i)
        {
            bool duplicate = false;
            for (std::size_t j = 0; j < uniquePaths.size(); ++j)
            {
                if (uniquePaths[j] == state_.waterAnimation.framePaths[i])
                {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate)
            {
                uniquePaths.push_back(state_.waterAnimation.framePaths[i]);
                uniqueNames.push_back(state_.waterAnimation.frameFileNames[i]);
            }
        }
        state_.waterAnimation.framePaths = std::move(uniquePaths);
        state_.waterAnimation.frameFileNames = std::move(uniqueNames);
        state_.waterAnimation.frameCount = static_cast<std::uint32_t>(state_.waterAnimation.framePaths.size());


        return state_.waterAnimation.frameCount > 0;
    }

    std::filesystem::path PhoenixRuntime::sky_texture_path() const
    {
        const std::string candidates[] = {
            state_.world.skyFileName,
            state_.world.primaryCloudFileName,
            state_.world.secondaryCloudFileName,
        };
        for (const auto& candidate : candidates)
        {
            auto path = texture_path_for(candidate);
            if (!path.empty())
                return path;
        }

        const auto skyName = phoenix::assets::lower_ascii(state_.world.skyFileName);
        if (skyName.find("a1") != std::string::npos)
        {
            auto path = state_.assets.resolve("L_A1_Skycloth_cloth.dds");
            if (!path.empty())
                return path;
        }
        if (skyName.find("b5") != std::string::npos)
        {
            auto path = state_.assets.resolve("b5_dun_sky01.dds");
            if (!path.empty())
                return path;
        }

        return state_.assets.resolve("skybox_SR.dds");
    }

    void PhoenixRuntime::build_terrain_mesh(
        std::vector<phoenix::renderer::TerrainVertex>& vertices,
        std::vector<std::uint32_t>& indices,
        TerrainLodInfo& lodInfo) const
    {
        vertices.clear();
        indices.clear();
        if (!state_.world.parsed || state_.world.heightSamples.empty() || state_.world.heightMapSide < 2)
            return;

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto halfMap = mapSize * 0.5f;
        const auto side = state_.world.heightMapSide;
        const auto grid = side - 1;
        const auto hasTextures = !state_.world.terrainLayers.empty()
            && !state_.world.terrainTextureMap.empty();
        const auto useGpuLookup = hasTextures;
        const auto totalQuads = static_cast<std::size_t>(grid) * grid;

        const auto stepWorld = mapSize / static_cast<float>(grid);
        const auto vertexSide = grid + 1;
        const auto vertexCount = static_cast<std::size_t>(vertexSide) * vertexSide;
        const auto texLayer = useGpuLookup ? 0xFFFFFFFDu : 0xFFFFFFFFu;

        vertices.clear();
        vertices.resize(vertexCount);
        indices.clear();
        indices.reserve(totalQuads * 6 + 12);

        for (std::uint32_t z = 0; z < vertexSide; ++z)
        {
            const auto wz = -halfMap + (static_cast<float>(z) / static_cast<float>(grid)) * mapSize;
            for (std::uint32_t x = 0; x < vertexSide; ++x)
            {
                const auto wx = -halfMap + (static_cast<float>(x) / static_cast<float>(grid)) * mapSize;
                const auto h = terrain_height(wx, wz);

                const auto hLeft = terrain_height(wx - stepWorld, wz);
                const auto hRight = terrain_height(wx + stepWorld, wz);
                const auto hDown = terrain_height(wx, wz - stepWorld);
                const auto hUp = terrain_height(wx, wz + stepWorld);
                float nx = hLeft - hRight;
                float ny = 2.0f * stepWorld;
                float nz = hDown - hUp;
                const auto nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (nLen > 0.001f) { nx /= nLen; ny /= nLen; nz /= nLen; }

                const auto high = std::clamp((h + 24.0f) / 150.0f, 0.0f, 1.0f);
                const auto water = h < 1.5f;

                auto& vertex = vertices[static_cast<std::size_t>(z) * vertexSide + x];
                vertex.position[0] = wx;
                vertex.position[1] = h;
                vertex.position[2] = wz;
                vertex.color[0] = water ? 0.03f : 0.20f + high * 0.26f;
                vertex.color[1] = water ? 0.14f + high * 0.06f : 0.42f + high * 0.22f;
                vertex.color[2] = water ? 0.32f + high * 0.10f : 0.18f + high * 0.12f;
                vertex.normal[0] = nx;
                vertex.normal[1] = ny;
                vertex.normal[2] = nz;
                vertex.uv[0] = (wx + halfMap) / 8.0f;
                vertex.uv[1] = (wz + halfMap) / 8.0f;
                vertex.textureLayer = texLayer;
            }
        }

        for (std::uint32_t z = 0; z < grid; ++z)
        {
            for (std::uint32_t x = 0; x < grid; ++x)
            {
                const auto a = z * vertexSide + x;
                const auto b = a + 1;
                const auto c = a + vertexSide;
                const auto d = c + 1;
                indices.push_back(a);
                indices.push_back(c);
                indices.push_back(b);
                indices.push_back(b);
                indices.push_back(c);
                indices.push_back(d);
            }
        }

        // ---- Generate per-chunk LOD index sets ----
        // The full-res indices are already in the buffer (stride 1). We now append
        // reduced-detail index sets for each chunk at strides 2, 4, 8. Each LOD
        // level skips quads, producing 1/4, 1/16, 1/64 of the triangles. The vertex
        // buffer stays the same — only indices change.
        {
            constexpr std::uint32_t kChunkQ = kTerrainChunkQuads;
            const auto chunkCountX = (grid + kChunkQ - 1u) / kChunkQ;
            const auto chunkCountZ = chunkCountX;
            lodInfo.chunkCountX = chunkCountX;
            lodInfo.chunkCountZ = chunkCountZ;
            lodInfo.grid = grid;
            lodInfo.cellSize = stepWorld;
            lodInfo.halfMap = halfMap;
            lodInfo.chunks.resize(static_cast<std::size_t>(chunkCountX) * chunkCountZ);

            const std::uint32_t strides[kTerrainLodLevels] = { 1, 2, 4, 8 };

            for (std::uint32_t cz = 0; cz < chunkCountZ; ++cz)
            {
                for (std::uint32_t cx = 0; cx < chunkCountX; ++cx)
                {
                    const auto chunkIdx = static_cast<std::size_t>(cz) * chunkCountX + cx;
                    const auto qMinX = cx * kChunkQ;
                    const auto qMinZ = cz * kChunkQ;
                    const auto qMaxX = std::min(grid, qMinX + kChunkQ);
                    const auto qMaxZ = std::min(grid, qMinZ + kChunkQ);

                    for (std::size_t lod = 0; lod < kTerrainLodLevels; ++lod)
                    {
                        const auto stride = strides[lod];
                        const auto firstIdx = static_cast<std::uint32_t>(indices.size());
                        for (std::uint32_t z = qMinZ; z < qMaxZ; z += stride)
                        {
                            const auto zNext = std::min(z + stride, qMaxZ);
                            for (std::uint32_t x = qMinX; x < qMaxX; x += stride)
                            {
                                const auto xNext = std::min(x + stride, qMaxX);
                                const auto a = z * vertexSide + x;
                                const auto b = z * vertexSide + xNext;
                                const auto c = zNext * vertexSide + x;
                                const auto d = zNext * vertexSide + xNext;
                                indices.push_back(a);
                                indices.push_back(c);
                                indices.push_back(b);
                                indices.push_back(b);
                                indices.push_back(c);
                                indices.push_back(d);
                            }
                        }
                        lodInfo.chunks[chunkIdx][lod].firstIndex = firstIdx;
                        lodInfo.chunks[chunkIdx][lod].indexCount =
                            static_cast<std::uint32_t>(indices.size()) - firstIdx;
                    }
                }
            }
        }

    }

    StaticObjectScene PhoenixRuntime::build_static_object_scene() const
    {
        StaticObjectScene scene;
        if (state_.worldAssets.empty() || state_.sceneObjects.empty())
            return scene;

        // Compatibility's object LOD gains most of its value by rejecting small
        // spatial groups before drawing.  Keep the original meshes intact, but
        // split each asset's instances into compact cells so frustum/indirect
        // culling can discard distant or off-screen decor as a unit.
        constexpr float kCellSize = 128.0f;
        struct CellGroup
        {
            std::int32_t cellX{};
            std::int32_t cellZ{};
            std::vector<std::size_t> objectIndices;
        };

        std::vector<std::vector<CellGroup>> groupsByAsset(state_.worldAssets.size());
        for (std::size_t objectIndex = 0; objectIndex < state_.sceneObjects.size(); ++objectIndex)
        {
            const auto& object = state_.sceneObjects[objectIndex];
            if (object.deleted || object.assetSlot < 0)
                continue;
            const auto assetSlot = static_cast<std::size_t>(object.assetSlot);
            if (assetSlot >= state_.worldAssets.size())
                continue;

            const auto& asset = state_.worldAssets[assetSlot];
            if (asset.vertexAnimated)
                continue;
            if (!object.loaded || asset.previewVertices.empty() || asset.previewIndices.empty())
                continue;

            const auto cellX = static_cast<std::int32_t>(std::floor(object.x / kCellSize));
            const auto cellZ = static_cast<std::int32_t>(std::floor(object.z / kCellSize));
            auto& groups = groupsByAsset[assetSlot];
            auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& group) {
                return group.cellX == cellX && group.cellZ == cellZ;
            });
            if (it == groups.end())
            {
                CellGroup group{};
                group.cellX = cellX;
                group.cellZ = cellZ;
                group.objectIndices.push_back(objectIndex);
                groups.push_back(std::move(group));
            }
            else
            {
                it->objectIndices.push_back(objectIndex);
            }
        }

        std::size_t vertexCount{};
        std::size_t indexCount{};
        std::size_t instanceCount{};
        std::size_t batchCount{};
        for (std::size_t assetSlot = 0; assetSlot < state_.worldAssets.size(); ++assetSlot)
        {
            if (groupsByAsset[assetSlot].empty())
                continue;

            const auto& asset = state_.worldAssets[assetSlot];
            vertexCount += asset.previewVertices.size();
            indexCount += asset.previewIndices.size();
            batchCount += groupsByAsset[assetSlot].size();
            for (const auto& group : groupsByAsset[assetSlot])
                instanceCount += group.objectIndices.size();
        }

        scene.vertices.reserve(vertexCount);
        scene.indices.reserve(indexCount);
        scene.instances.reserve(instanceCount);
        scene.batches.reserve(batchCount);
        scene.batchBounds.reserve(batchCount);

        const auto appendInstance = [&](const SceneObject& object) {
            // Zero means there is no per-asset cap: the universal fog/view
            // distance is the single render boundary for every world asset.
            scene.instances.push_back(make_object_instance(object, 0.0f));
        };

        for (std::size_t assetSlot = 0; assetSlot < state_.worldAssets.size(); ++assetSlot)
        {
            auto& groups = groupsByAsset[assetSlot];
            if (groups.empty())
                continue;

            std::ranges::sort(groups, [](const auto& lhs, const auto& rhs) {
                if (lhs.cellZ != rhs.cellZ) return lhs.cellZ < rhs.cellZ;
                return lhs.cellX < rhs.cellX;
            });

            const auto& asset = state_.worldAssets[assetSlot];

            const auto baseVertex = static_cast<std::uint32_t>(scene.vertices.size());
            const auto firstIndex = static_cast<std::uint32_t>(scene.indices.size());
            scene.vertices.insert(scene.vertices.end(), asset.previewVertices.begin(), asset.previewVertices.end());
            for (const auto index : asset.previewIndices)
                scene.indices.push_back(baseVertex + index);

            // One batch per compact spatial cell. The indirect path consumes all
            // of these in one GPU submission; the fallback path only emits the
            // cells that pass the same bounds test.
            for (const auto& group : groups)
            {
                const auto firstInstance = static_cast<std::uint32_t>(scene.instances.size());
                float minX = std::numeric_limits<float>::max();
                float minY = std::numeric_limits<float>::max();
                float minZ = std::numeric_limits<float>::max();
                float maxX = -std::numeric_limits<float>::max();
                float maxY = -std::numeric_limits<float>::max();
                float maxZ = -std::numeric_limits<float>::max();
                for (const auto objectIndex : group.objectIndices)
                {
                    const auto& object = state_.sceneObjects[objectIndex];
                    appendInstance(object);
                    const auto radius = std::max(8.0f, object.radius);
                    minX = std::min(minX, object.x - radius);
                    minY = std::min(minY, object.y - radius);
                    minZ = std::min(minZ, object.z - radius);
                    maxX = std::max(maxX, object.x + radius);
                    maxY = std::max(maxY, object.y + radius);
                    maxZ = std::max(maxZ, object.z + radius);
                }

                StaticObjectScene::BatchBounds bounds{};
                bounds.x = (minX + maxX) * 0.5f;
                bounds.y = (minY + maxY) * 0.5f;
                bounds.z = (minZ + maxZ) * 0.5f;
                const auto extentX = (maxX - minX) * 0.5f;
                const auto extentY = (maxY - minY) * 0.5f;
                const auto extentZ = (maxZ - minZ) * 0.5f;
                bounds.radius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);

                phoenix::renderer::ObjectBatch batch{};
                batch.firstIndex = firstIndex;
                batch.indexCount = static_cast<std::uint32_t>(asset.previewIndices.size());
                batch.firstInstance = firstInstance;
                batch.instanceCount = static_cast<std::uint32_t>(scene.instances.size()) - firstInstance;
                scene.batches.push_back(batch);
                scene.batchBounds.push_back(bounds);
            }
        }

        return scene;
    }

    AnimatedObjectScene PhoenixRuntime::build_animated_object_scene() const
    {
        AnimatedObjectScene scene;
        if (state_.worldAssets.empty() || state_.sceneObjects.empty())
            return scene;

        std::vector<std::vector<std::size_t>> groupsByAsset(state_.worldAssets.size());
        for (std::size_t objectIndex = 0; objectIndex < state_.sceneObjects.size(); ++objectIndex)
        {
            const auto& object = state_.sceneObjects[objectIndex];
            if (object.deleted || object.assetSlot < 0)
                continue;
            const auto assetSlot = static_cast<std::size_t>(object.assetSlot);
            if (assetSlot >= state_.worldAssets.size())
                continue;
            const auto& asset = state_.worldAssets[assetSlot];
            if (!object.loaded || asset.previewVertices.empty() || asset.previewIndices.empty())
                continue;
            if (asset.vertexAnimated)
                groupsByAsset[assetSlot].push_back(objectIndex);
        }

        const auto appendInstance = [&](const SceneObject& object) {
            const auto instance = make_object_instance(object, 0.0f);
            scene.baseInstances.push_back(instance);
            scene.instances.push_back(instance);
        };

        for (std::size_t assetSlot = 0; assetSlot < state_.worldAssets.size(); ++assetSlot)
        {
            const auto& objectIndices = groupsByAsset[assetSlot];
            if (objectIndices.empty())
                continue;

            const auto& asset = state_.worldAssets[assetSlot];
            const auto baseVertex = static_cast<std::uint32_t>(scene.vertices.size());
            const auto firstIndex = static_cast<std::uint32_t>(scene.indices.size());
            scene.vertices.insert(scene.vertices.end(), asset.previewVertices.begin(), asset.previewVertices.end());
            for (const auto index : asset.previewIndices)
                scene.indices.push_back(baseVertex + index);

            if (asset.vertexAnimated && asset.animationFrames
                && asset.animationFrames->size() == asset.frameCount)
            {
                AnimatedObjectScene::VertexAnimation animation{};
                animation.firstVertex = baseVertex;
                animation.vertexCount = static_cast<std::uint32_t>(asset.previewVertices.size());
                animation.firstIndex = firstIndex;
                animation.indexCount = static_cast<std::uint32_t>(asset.previewIndices.size());
                animation.firstInstance = static_cast<std::uint32_t>(scene.instances.size());
                animation.instanceCount = static_cast<std::uint32_t>(objectIndices.size());
                animation.frames = asset.animationFrames; // shared, no copy
                scene.vertexAnimations.push_back(std::move(animation));
            }

            phoenix::renderer::ObjectBatch batch{};
            batch.firstIndex = firstIndex;
            batch.indexCount = static_cast<std::uint32_t>(asset.previewIndices.size());
            batch.firstInstance = static_cast<std::uint32_t>(scene.instances.size());
            batch.instanceCount = static_cast<std::uint32_t>(objectIndices.size());

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();

            for (const auto objectIndex : objectIndices)
            {
                const auto& object = state_.sceneObjects[objectIndex];
                appendInstance(object);

                const auto radius = std::max(8.0f, object.radius);
                minX = std::min(minX, object.x - radius);
                minY = std::min(minY, object.y - radius);
                minZ = std::min(minZ, object.z - radius);
                maxX = std::max(maxX, object.x + radius);
                maxY = std::max(maxY, object.y + radius);
                maxZ = std::max(maxZ, object.z + radius);
            }

            StaticObjectScene::BatchBounds bounds{};
            bounds.x = (minX + maxX) * 0.5f;
            bounds.y = (minY + maxY) * 0.5f;
            bounds.z = (minZ + maxZ) * 0.5f;
            const auto extentX = (maxX - minX) * 0.5f;
            const auto extentY = (maxY - minY) * 0.5f;
            const auto extentZ = (maxZ - minZ) * 0.5f;
            bounds.radius = std::sqrt(extentX * extentX + extentY * extentY + extentZ * extentZ);
            scene.batches.push_back(batch);
            scene.batchBounds.push_back(bounds);
        }

        return scene;
    }

    void PhoenixRuntime::update_animated_object_scene(AnimatedObjectScene& scene, float totalTime,
        float cameraX, float cameraY, float cameraZ, float viewDistance) const
    {
        constexpr float kDecorFps = 12.0f;
        const float animateDistanceSq = viewDistance * viewDistance;

        for (auto& animation : scene.vertexAnimations)
        {
            if (!animation.visible || !animation.frames || animation.frames->empty())
                continue;

            // Animate only if the nearest instance of this asset is close enough.
            bool anyInstanceNear = false;
            const auto instanceEnd = std::min<std::size_t>(
                static_cast<std::size_t>(animation.firstInstance) + animation.instanceCount,
                scene.baseInstances.size());
            for (std::size_t i = animation.firstInstance; i < instanceEnd; ++i)
            {
                const auto& position = scene.baseInstances[i].position;
                const float dx = position[0] - cameraX;
                const float dy = position[1] - cameraY;
                const float dz = position[2] - cameraZ;
                if (dx * dx + dy * dy + dz * dz <= animateDistanceSq)
                {
                    anyInstanceNear = true;
                    break;
                }
            }
            if (!anyInstanceNear)
                continue;

            const auto frame = static_cast<std::uint32_t>(
                static_cast<std::size_t>(std::floor(totalTime * kDecorFps)) % animation.frames->size());
            if (frame == animation.currentFrame)
                continue;
            animation.currentFrame = frame;
            const auto& frameVertices = (*animation.frames)[frame];
            const auto count = std::min<std::size_t>(animation.vertexCount, frameVertices.size());
            if (static_cast<std::size_t>(animation.firstVertex) + count <= scene.vertices.size())
            {
                std::copy_n(frameVertices.begin(), count, scene.vertices.begin() + animation.firstVertex);
                scene.mark_vertices_dirty(animation.firstVertex, static_cast<std::uint32_t>(count));
            }
        }
    }

    void PhoenixRuntime::camera_state(float& x, float& y, float& z, float& yaw, float& pitch) const
    {
        x = camera_.x;
        y = camera_.y;
        z = camera_.z;
        yaw = camera_.yaw;
        pitch = camera_.pitch;
    }

    void PhoenixRuntime::set_camera_position(float x, float y, float z, float yaw, float pitch)
    {
        camera_.x = x;
        camera_.y = y;
        camera_.z = z;
        camera_.yaw = yaw;
        camera_.pitch = pitch;
    }

    void PhoenixRuntime::update_camera(float deltaSeconds, const CameraInput& input)
    {
        if (input.look)
        {
            camera_.yaw -= input.mouseDx * 0.003f;
            camera_.pitch = std::clamp(camera_.pitch - input.mouseDy * 0.003f, -1.5f, 1.5f);
        }
        if (input.yawLeft)
            camera_.yaw -= deltaSeconds * 1.8f;
        if (input.yawRight)
            camera_.yaw += deltaSeconds * 1.8f;
        if (input.pitchUp)
            camera_.pitch = std::clamp(camera_.pitch - deltaSeconds * 1.4f, -1.5f, 1.5f);
        if (input.pitchDown)
            camera_.pitch = std::clamp(camera_.pitch + deltaSeconds * 1.4f, -1.5f, 1.5f);

        if (input.wheel != 0.0f)
            camera_.speed = std::clamp(camera_.speed * std::pow(1.2f, input.wheel), 2.0f, 2000.0f);
        const auto speed = camera_.speed * (input.fast ? 6.0f : 1.0f) * std::max(0.0f, deltaSeconds);
        const auto cy = std::cos(camera_.yaw);
        const auto sy = std::sin(camera_.yaw);
        const auto cp = std::cos(camera_.pitch);
        const auto sp = std::sin(camera_.pitch);
        const auto forwardX = sy;
        const auto forwardY = sp;
        const auto forwardZ = cp * cy;
        const auto rightX = -cy;
        const auto rightZ = sy;

        float moveX = 0.0f;
        float moveY = 0.0f;
        float moveZ = 0.0f;
        if (input.forward)
        {
            moveX += cp * forwardX;
            moveY += forwardY;
            moveZ += forwardZ;
        }
        if (input.backward)
        {
            moveX -= cp * forwardX;
            moveY -= forwardY;
            moveZ -= forwardZ;
        }
        if (input.right)
        {
            moveX += rightX;
            moveZ += rightZ;
        }
        if (input.left)
        {
            moveX -= rightX;
            moveZ -= rightZ;
        }
        if (input.up)
            moveY += 1.0f;
        if (input.down)
            moveY -= 1.0f;

        const auto moveLength = std::sqrt(moveX * moveX + moveY * moveY + moveZ * moveZ);
        if (moveLength > 0.001f)
        {
            moveX /= moveLength;
            moveY /= moveLength;
            moveZ /= moveLength;
        }
        camera_.x += moveX * speed;
        camera_.y += moveY * speed;
        camera_.z += moveZ * speed;

        if (state_.world.isDungeon)
        {
            constexpr float kDungeonBounds = 10000.0f;
            camera_.x = std::clamp(camera_.x, -kDungeonBounds, kDungeonBounds);
            camera_.z = std::clamp(camera_.z, -kDungeonBounds, kDungeonBounds);
            camera_.y = std::clamp(camera_.y, -kDungeonBounds, kDungeonBounds);
        }
        else
        {
            const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
            camera_.x = std::clamp(camera_.x, -mapSize, mapSize);
            camera_.z = std::clamp(camera_.z, -mapSize, mapSize);
            camera_.y = std::clamp(camera_.y, -10000.0f, 500.0f);
        }
    }

    std::string PhoenixRuntime::window_title(const std::string& /*rendererName*/, float /*fps*/, bool /*fogEnabled*/) const
    {
        return "Phoenix Engine";
    }

    // ---- World collision mesh ----

    void WorldCollisionMesh::build_grid()
    {
        grid.clear();
        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(triangles.size()); ++i)
        {
            const auto& tri = triangles[i];
            // Find XZ bounding box of the triangle.
            float minX = std::min({ tri.v0[0], tri.v1[0], tri.v2[0] });
            float maxX = std::max({ tri.v0[0], tri.v1[0], tri.v2[0] });
            float minZ = std::min({ tri.v0[2], tri.v1[2], tri.v2[2] });
            float maxZ = std::max({ tri.v0[2], tri.v1[2], tri.v2[2] });
            int cx0 = static_cast<int>(std::floor(minX / kCellSize));
            int cx1 = static_cast<int>(std::floor(maxX / kCellSize));
            int cz0 = static_cast<int>(std::floor(minZ / kCellSize));
            int cz1 = static_cast<int>(std::floor(maxZ / kCellSize));
            for (int cx = cx0; cx <= cx1; ++cx)
                for (int cz = cz0; cz <= cz1; ++cz)
                    grid[cell_key(cx, cz)].push_back(i);
        }
    }

    namespace
    {
        // 2D (XZ) point-in-triangle test.
        float cross2d(float ax, float az, float bx, float bz)
        {
            return ax * bz - az * bx;
        }

        bool point_in_triangle_xz(float px, float pz,
            const float* v0, const float* v1, const float* v2)
        {
            float d1 = cross2d(v1[0] - v0[0], v1[2] - v0[2], px - v0[0], pz - v0[2]);
            float d2 = cross2d(v2[0] - v1[0], v2[2] - v1[2], px - v1[0], pz - v1[2]);
            float d3 = cross2d(v0[0] - v2[0], v0[2] - v2[2], px - v2[0], pz - v2[2]);
            bool hasNeg = (d1 < 0) || (d2 < 0) || (d3 < 0);
            bool hasPos = (d1 > 0) || (d2 > 0) || (d3 > 0);
            return !(hasNeg && hasPos);
        }

        // Closest point on a 2D line segment (XZ) to a point.
        void closest_point_on_segment_xz(float px, float pz,
            float ax, float az, float bx, float bz,
            float& outX, float& outZ)
        {
            float dx = bx - ax;
            float dz = bz - az;
            float lenSq = dx * dx + dz * dz;
            if (lenSq < 0.0001f) { outX = ax; outZ = az; return; }
            float t = std::clamp(((px - ax) * dx + (pz - az) * dz) / lenSq, 0.0f, 1.0f);
            outX = ax + t * dx;
            outZ = az + t * dz;
        }
    }

    // Find the closest point on any collision triangle to a point in XZ.
    // Returns the penetration depth (negative if outside radius).
    namespace
    {
        struct CollisionContact
        {
            float nearX{};
            float nearZ{};
            float distSq{ std::numeric_limits<float>::max() };
            bool inside{}; // true if point is inside the triangle in XZ
        };

        CollisionContact closest_triangle_contact(float px, float pz, const WorldCollisionMesh::Triangle& tri)
        {
            CollisionContact result{};
            result.inside = point_in_triangle_xz(px, pz, tri.v0, tri.v1, tri.v2);

            // Always find closest point on the three edges.
            const float* edges[3][2] = {
                { tri.v0, tri.v1 }, { tri.v1, tri.v2 }, { tri.v2, tri.v0 }
            };
            for (const auto& edge : edges)
            {
                float ex, ez;
                closest_point_on_segment_xz(px, pz,
                    edge[0][0], edge[0][2], edge[1][0], edge[1][2], ex, ez);
                float dx = px - ex;
                float dz = pz - ez;
                float d = dx * dx + dz * dz;
                if (d < result.distSq)
                {
                    result.distSq = d;
                    result.nearX = ex;
                    result.nearZ = ez;
                }
            }
            return result;
        }
    }

    bool WorldCollisionMesh::check_collision(float prevX, float prevZ,
        float& proposedX, float& proposedZ,
        float characterY, float characterHeight, float characterRadius) const
    {
        bool collided = false;
        const float radiusSq = characterRadius * characterRadius;

        // Character vertical range.
        const float charMinY = characterY;
        const float charMaxY = characterY + characterHeight;

        // Maximum displacement allowed - prevents teleportation.
        const float moveDx = proposedX - prevX;
        const float moveDz = proposedZ - prevZ;
        const float maxDisplacementSq = (moveDx * moveDx + moveDz * moveDz) * 4.0f + 1.0f;

        // Multiple passes to resolve stacking collisions.
        for (int pass = 0; pass < 3; ++pass)
        {
            float deepestPen = 0.0f;
            float pushNx = 0.0f;
            float pushNz = 0.0f;
            float pushAmount = 0.0f;

            const int cx = static_cast<int>(std::floor(proposedX / kCellSize));
            const int cz = static_cast<int>(std::floor(proposedZ / kCellSize));

            for (int dcx = -1; dcx <= 1; ++dcx)
            {
                for (int dcz = -1; dcz <= 1; ++dcz)
                {
                    auto it = grid.find(cell_key(cx + dcx, cz + dcz));
                    if (it == grid.end())
                        continue;
                    for (const auto triIndex : it->second)
                    {
                        const auto& tri = triangles[triIndex];

                        // Skip triangles that don't overlap the character's vertical range.
                        if (tri.minY > charMaxY || tri.maxY < charMinY)
                            continue;

                        // Skip walkable (floor-like) triangles - these are handled as
                        // elevated terrain via floor_height_at, not as walls.
                        if (tri.normalY >= kWalkableNormalY)
                            continue;

                        // Step-up forgiveness: skip wall triangles whose top is within
                        // step-up range of the character's feet. This lets the character
                        // walk onto ramps/bridges without needing to jump over the edge.
                        constexpr float kStepUpTolerance = 1.5f;
                        if (tri.maxY <= charMinY + kStepUpTolerance && tri.maxY >= charMinY)
                            continue;

                        const auto contact = closest_triangle_contact(proposedX, proposedZ, tri);

                        if (contact.inside)
                        {
                            // Inside the triangle - must push out.
                            // Push direction: from nearest edge point OUTWARD (away from triangle center).
                            float dist = std::sqrt(contact.distSq);
                            float penetration = characterRadius + dist; // full push past the edge
                            if (penetration > deepestPen)
                            {
                                deepestPen = penetration;
                                if (dist > 0.001f)
                                {
                                    // Push from the edge point toward the character.
                                    // But since we're INSIDE, the direction from nearest-edge to us
                                    // points inward. We want to push OUTWARD = toward prev position.
                                    float toPrevX = prevX - proposedX;
                                    float toPrevZ = prevZ - proposedZ;
                                    float toPrevLen = std::sqrt(toPrevX * toPrevX + toPrevZ * toPrevZ);
                                    if (toPrevLen > 0.001f)
                                    {
                                        pushNx = toPrevX / toPrevLen;
                                        pushNz = toPrevZ / toPrevLen;
                                    }
                                    else
                                    {
                                        // No movement direction - push away from edge.
                                        pushNx = (proposedX - contact.nearX) / dist;
                                        pushNz = (proposedZ - contact.nearZ) / dist;
                                    }
                                }
                                pushAmount = penetration;
                            }
                        }
                        else if (contact.distSq < radiusSq)
                        {
                            // Outside but within radius - gentle push.
                            float dist = std::sqrt(contact.distSq);
                            float penetration = characterRadius - dist;
                            if (penetration > deepestPen && dist > 0.001f)
                            {
                                deepestPen = penetration;
                                pushNx = (proposedX - contact.nearX) / dist;
                                pushNz = (proposedZ - contact.nearZ) / dist;
                                pushAmount = penetration;
                            }
                        }
                    }
                }
            }

            if (deepestPen <= 0.001f)
                break;

            // Apply the single deepest push.
            proposedX += pushNx * pushAmount;
            proposedZ += pushNz * pushAmount;
            collided = true;

            // Safety: if we've moved too far from original, snap back.
            float totalDx = proposedX - prevX;
            float totalDz = proposedZ - prevZ;
            if (totalDx * totalDx + totalDz * totalDz > maxDisplacementSq)
            {
                proposedX = prevX;
                proposedZ = prevZ;
                break;
            }
        }

        return collided;
    }

    float WorldCollisionMesh::floor_height_at(float worldX, float worldZ,
        float characterY, float stepHeight) const
    {
        float bestY = -99999.0f;
        const float maxY = characterY + stepHeight; // can step up this high

        const int cx = static_cast<int>(std::floor(worldX / kCellSize));
        const int cz = static_cast<int>(std::floor(worldZ / kCellSize));

        for (int dcx = -1; dcx <= 1; ++dcx)
        {
            for (int dcz = -1; dcz <= 1; ++dcz)
            {
                auto it = grid.find(cell_key(cx + dcx, cz + dcz));
                if (it == grid.end())
                    continue;
                for (const auto triIndex : it->second)
                {
                    const auto& tri = triangles[triIndex];

                    // Only consider walkable (floor-like) triangles.
                    if (tri.normalY < kWalkableNormalY)
                        continue;

                    // Quick Y range check - triangle must be reachable.
                    if (tri.minY > maxY || tri.maxY < bestY)
                        continue;

                    // Check if point is inside the triangle in XZ.
                    if (!point_in_triangle_xz(worldX, worldZ, tri.v0, tri.v1, tri.v2))
                        continue;

                    // Interpolate Y at (worldX, worldZ) using barycentric coordinates.
                    float e1x = tri.v1[0] - tri.v0[0], e1z = tri.v1[2] - tri.v0[2];
                    float e2x = tri.v2[0] - tri.v0[0], e2z = tri.v2[2] - tri.v0[2];
                    float det = e1x * e2z - e2x * e1z;
                    if (std::abs(det) < 0.0001f)
                        continue;
                    float invDet = 1.0f / det;
                    float dx = worldX - tri.v0[0], dz = worldZ - tri.v0[2];
                    float u = (dx * e2z - e2x * dz) * invDet;
                    float v = (e1x * dz - dx * e1z) * invDet;
                    if (u < 0.0f || v < 0.0f || u + v > 1.0f)
                        continue; // degenerate / outside (numerical edge case)

                    float surfaceY = tri.v0[1] + u * (tri.v1[1] - tri.v0[1]) + v * (tri.v2[1] - tri.v0[1]);

                    // Surface must be below the step threshold and above current best.
                    if (surfaceY <= maxY && surfaceY > bestY)
                        bestY = surfaceY;
                }
            }
        }

        return bestY;
    }

    std::vector<PhoenixRuntime::LadderVolume> PhoenixRuntime::ladder_volumes() const
    {
        std::vector<LadderVolume> volumes;
        for (const auto& obj : state_.sceneObjects)
        {
            if (obj.deleted || obj.assetSlot < 0)
                continue;
            if (obj.sectionIndex < 0
                || static_cast<std::size_t>(obj.sectionIndex) >= state_.world.objectSections.size()
                || state_.world.objectSections[static_cast<std::size_t>(obj.sectionIndex)].name != "Object")
                continue;
            const auto slot = static_cast<std::size_t>(obj.assetSlot);
            if (slot >= state_.worldAssets.size())
                continue;
            const auto& asset = state_.worldAssets[slot];
            if (asset.previewVertices.empty())
                continue;

            // Local AABB of the climbable mesh.
            float minL[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            float maxL[3]{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
            for (const auto& vertex : asset.previewVertices)
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    minL[axis] = std::min(minL[axis], vertex.position[axis]);
                    maxL[axis] = std::max(maxL[axis], vertex.position[axis]);
                }
            }

            // World AABB: transform the 8 local corners by the instance basis.
            float forward[3]{ obj.forward[0], obj.forward[1], obj.forward[2] };
            float up[3]{ obj.up[0], obj.up[1], obj.up[2] };
            auto normalize = [](float* v, float fx, float fy, float fz) {
                const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                if (len < 0.001f) { v[0] = fx; v[1] = fy; v[2] = fz; return; }
                v[0] /= len; v[1] /= len; v[2] /= len;
            };
            normalize(forward, 0.0f, 0.0f, 1.0f);
            normalize(up, 0.0f, 1.0f, 0.0f);
            const float right[3]{
                up[1] * forward[2] - up[2] * forward[1],
                up[2] * forward[0] - up[0] * forward[2],
                up[0] * forward[1] - up[1] * forward[0],
            };

            float minW[3]{ std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
            float maxW[3]{ std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
            for (int corner = 0; corner < 8; ++corner)
            {
                const float lx = (corner & 1) ? maxL[0] : minL[0];
                const float ly = (corner & 2) ? maxL[1] : minL[1];
                const float lz = (corner & 4) ? maxL[2] : minL[2];
                const float wx = obj.x + right[0] * lx + up[0] * ly + forward[0] * lz;
                const float wy = obj.y + right[1] * lx + up[1] * ly + forward[1] * lz;
                const float wz = obj.z + right[2] * lx + up[2] * ly + forward[2] * lz;
                minW[0] = std::min(minW[0], wx); maxW[0] = std::max(maxW[0], wx);
                minW[1] = std::min(minW[1], wy); maxW[1] = std::max(maxW[1], wy);
                minW[2] = std::min(minW[2], wz); maxW[2] = std::max(maxW[2], wz);
            }

            LadderVolume volume{};
            volume.x = (minW[0] + maxW[0]) * 0.5f;
            volume.z = (minW[2] + maxW[2]) * 0.5f;
            volume.baseY = minW[1];
            volume.topY = maxW[1];
            // Capture radius: the ladder's horizontal footprint plus a small
            // approach margin so walking up to it latches reliably.
            const float extentX = (maxW[0] - minW[0]) * 0.5f;
            const float extentZ = (maxW[2] - minW[2]) * 0.5f;
            volume.radius = std::max(extentX, extentZ) + 0.9f;
            if (volume.topY - volume.baseY >= 1.5f) // ignore flat/short objects
                volumes.push_back(volume);
        }
        return volumes;
    }

    bool WorldCollisionMesh::segment_occluded(const float a[3], const float b[3]) const
    {
        if (triangles.empty() || grid.empty())
            return false;

        const float dir[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
        const int cx0 = static_cast<int>(std::floor(std::min(a[0], b[0]) / kCellSize));
        const int cx1 = static_cast<int>(std::floor(std::max(a[0], b[0]) / kCellSize));
        const int cz0 = static_cast<int>(std::floor(std::min(a[2], b[2]) / kCellSize));
        const int cz1 = static_cast<int>(std::floor(std::max(a[2], b[2]) / kCellSize));
        // Callers pass short segments; bail on a pathologically large span.
        if ((cx1 - cx0) > 64 || (cz1 - cz0) > 64)
            return false;

        // Margins skip the camera's immediate surroundings and the triangle right
        // under the NPC so neither self-occludes the label.
        constexpr float kTMin = 0.03f;
        constexpr float kTMax = 0.985f;
        constexpr float kEps = 1e-7f;

        for (int cx = cx0; cx <= cx1; ++cx)
        {
            for (int cz = cz0; cz <= cz1; ++cz)
            {
                const auto it = grid.find(cell_key(cx, cz));
                if (it == grid.end())
                    continue;
                for (const auto idx : it->second)
                {
                    const auto& tri = triangles[idx];
                    // Möller–Trumbore against the segment (dir = b - a, t in [0,1]).
                    const float e1[3] = { tri.v1[0] - tri.v0[0], tri.v1[1] - tri.v0[1], tri.v1[2] - tri.v0[2] };
                    const float e2[3] = { tri.v2[0] - tri.v0[0], tri.v2[1] - tri.v0[1], tri.v2[2] - tri.v0[2] };
                    const float pvec[3] = {
                        dir[1] * e2[2] - dir[2] * e2[1],
                        dir[2] * e2[0] - dir[0] * e2[2],
                        dir[0] * e2[1] - dir[1] * e2[0],
                    };
                    const float det = e1[0] * pvec[0] + e1[1] * pvec[1] + e1[2] * pvec[2];
                    if (det > -kEps && det < kEps)
                        continue;
                    const float inv = 1.0f / det;
                    const float tvec[3] = { a[0] - tri.v0[0], a[1] - tri.v0[1], a[2] - tri.v0[2] };
                    const float u = (tvec[0] * pvec[0] + tvec[1] * pvec[1] + tvec[2] * pvec[2]) * inv;
                    if (u < 0.0f || u > 1.0f)
                        continue;
                    const float qvec[3] = {
                        tvec[1] * e1[2] - tvec[2] * e1[1],
                        tvec[2] * e1[0] - tvec[0] * e1[2],
                        tvec[0] * e1[1] - tvec[1] * e1[0],
                    };
                    const float v = (dir[0] * qvec[0] + dir[1] * qvec[1] + dir[2] * qvec[2]) * inv;
                    if (v < 0.0f || u + v > 1.0f)
                        continue;
                    const float t = (e2[0] * qvec[0] + e2[1] * qvec[1] + e2[2] * qvec[2]) * inv;
                    if (t > kTMin && t < kTMax)
                        return true;
                }
            }
        }
        return false;
    }

    WorldCollisionMesh PhoenixRuntime::build_collision_mesh() const
    {
        WorldCollisionMesh mesh;

        auto normalize = [](float* v) {
            float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (len > 0.0001f) { v[0] /= len; v[1] /= len; v[2] /= len; }
        };

        for (const auto& obj : state_.sceneObjects)
        {
            if (obj.deleted || obj.assetSlot < 0)
                continue;
            const auto slot = static_cast<std::size_t>(obj.assetSlot);
            if (slot >= state_.worldAssets.size())
                continue;
            // "Object" section assets (ladders/ivy in entity/object/) never
            // collide — the character latches onto them and climbs instead.
            if (obj.sectionIndex >= 0
                && static_cast<std::size_t>(obj.sectionIndex) < state_.world.objectSections.size()
                && state_.world.objectSections[static_cast<std::size_t>(obj.sectionIndex)].name == "Object")
                continue;
            const auto& asset = state_.worldAssets[slot];
            if (!asset.hasCollision || asset.collisionVertices.empty() || asset.collisionIndices.empty())
                continue;

            // Build transform matrix from SceneObject (same as appendInstance).
            float forward[3]{ obj.forward[0], obj.forward[1], obj.forward[2] };
            float up[3]{ obj.up[0], obj.up[1], obj.up[2] };
            normalize(forward);
            normalize(up);
            float right[3]{
                up[1] * forward[2] - up[2] * forward[1],
                up[2] * forward[0] - up[0] * forward[2],
                up[0] * forward[1] - up[1] * forward[0],
            };

            const auto vertexCount = asset.collisionVertices.size() / 3;
            // Transform collision vertices to world space.
            std::vector<float> worldVerts(asset.collisionVertices.size());
            for (std::size_t v = 0; v < vertexCount; ++v)
            {
                const float lx = asset.collisionVertices[v * 3 + 0];
                const float ly = asset.collisionVertices[v * 3 + 1];
                const float lz = asset.collisionVertices[v * 3 + 2];
                worldVerts[v * 3 + 0] = obj.x + right[0] * lx + up[0] * ly + forward[0] * lz;
                worldVerts[v * 3 + 1] = obj.y + right[1] * lx + up[1] * ly + forward[1] * lz;
                worldVerts[v * 3 + 2] = obj.z + right[2] * lx + up[2] * ly + forward[2] * lz;
            }

            // Add triangles.
            const auto faceCount = asset.collisionIndices.size() / 3;
            for (std::size_t f = 0; f < faceCount; ++f)
            {
                const auto i0 = asset.collisionIndices[f * 3 + 0];
                const auto i1 = asset.collisionIndices[f * 3 + 1];
                const auto i2 = asset.collisionIndices[f * 3 + 2];
                if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount)
                    continue;
                WorldCollisionMesh::Triangle tri{};
                tri.v0[0] = worldVerts[i0 * 3 + 0]; tri.v0[1] = worldVerts[i0 * 3 + 1]; tri.v0[2] = worldVerts[i0 * 3 + 2];
                tri.v1[0] = worldVerts[i1 * 3 + 0]; tri.v1[1] = worldVerts[i1 * 3 + 1]; tri.v1[2] = worldVerts[i1 * 3 + 2];
                tri.v2[0] = worldVerts[i2 * 3 + 0]; tri.v2[1] = worldVerts[i2 * 3 + 1]; tri.v2[2] = worldVerts[i2 * 3 + 2];
                tri.minY = std::min({ tri.v0[1], tri.v1[1], tri.v2[1] });
                tri.maxY = std::max({ tri.v0[1], tri.v1[1], tri.v2[1] });
                // Compute face normal Y component for slope classification.
                float e1x = tri.v1[0] - tri.v0[0], e1y = tri.v1[1] - tri.v0[1], e1z = tri.v1[2] - tri.v0[2];
                float e2x = tri.v2[0] - tri.v0[0], e2y = tri.v2[1] - tri.v0[1], e2z = tri.v2[2] - tri.v0[2];
                float nx = e1y * e2z - e1z * e2y;
                float ny = e1z * e2x - e1x * e2z;
                float nz = e1x * e2y - e1y * e2x;
                float nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
                tri.normalY = (nLen > 0.0001f) ? std::abs(ny) / nLen : 0.0f;
                mesh.triangles.push_back(tri);
            }
        }

        mesh.build_grid();

        {
        }

        return mesh;
    }
}
