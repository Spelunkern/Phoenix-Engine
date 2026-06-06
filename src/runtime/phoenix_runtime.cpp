#define _CRT_SECURE_NO_WARNINGS
#include "runtime/phoenix_runtime.h"

#include "assets/data_index.h"
#include "world/dg_loader.h"
#include "world/smod_loader.h"
#include "world/vani_loader.h"
#include "world/phoenix_world_loader.h"
#include "world/water_constants.h"

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
#include <numeric>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace phoenix::runtime
{
    namespace
    {
        constexpr std::uint32_t kAssetTextureLayerBase = 66;
        constexpr std::uint32_t kAssetCutoutLayerBase = 2048;
        constexpr std::uint32_t kMaxAssetTextureLayers = 960;

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

        bool static_texture_uses_cutout(std::string_view textureName)
        {
            const auto lowerName = phoenix::assets::lower_ascii(std::string(textureName));
            constexpr std::string_view cutoutTokens[] = {
                "leaf", "leav", "tree", "grass", "bush", "flower", "plant",
                "weed", "branch", "vine", "fern", "shrub",
            };
            for (const auto token : cutoutTokens)
            {
                if (lowerName.find(token) != std::string::npos)
                    return true;
            }
            return false;
        }

        bool static_asset_uses_cutout(std::string_view assetName, const std::filesystem::path& assetPath)
        {
            auto lowerName = phoenix::assets::lower_ascii(std::string(assetName));
            auto lowerPath = phoenix::assets::lower_ascii(assetPath.string());
            std::ranges::replace(lowerPath, '\\', '/');
            constexpr std::string_view cutoutTokens[] = {
                "/tree/", "/grass/",
                "tree", "leaf", "leav", "bush", "plant", "grass",
                "branch", "vine", "fern", "shrub",
            };
            for (const auto token : cutoutTokens)
            {
                if (lowerName.find(token) != std::string::npos || lowerPath.find(token) != std::string::npos)
                    return true;
            }
            return false;
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

        std::uint32_t read_le_u32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 4 > data.size())
                return 0;
            return static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

        std::array<float, 3> rgb565(std::uint16_t value)
        {
            return {
                static_cast<float>((value >> 11) & 0x1F) / 31.0f,
                static_cast<float>((value >> 5) & 0x3F) / 63.0f,
                static_cast<float>(value & 0x1F) / 31.0f,
            };
        }

        std::array<float, 3> average_dds_bc_color(const std::filesystem::path& path)
        {
            auto data = assets::read_file_binary(path);
            if (data.size() < 128 || read_le_u32(data, 0) != 0x20534444u)
                return { 0.34f, 0.48f, 0.22f };

            const auto height = read_le_u32(data, 12);
            const auto width = read_le_u32(data, 16);
            const auto fourCc = read_le_u32(data, 84);
            const auto isDxt1 = fourCc == 0x31545844u;
            const auto isDxt3 = fourCc == 0x33545844u;
            const auto isDxt5 = fourCc == 0x35545844u;
            if (width == 0 || height == 0 || (!isDxt1 && !isDxt3 && !isDxt5))
                return { 0.34f, 0.48f, 0.22f };

            const auto blockBytes = isDxt1 ? 8u : 16u;
            const auto blocksWide = std::max<std::uint32_t>(1, (width + 3) / 4);
            const auto blocksHigh = std::max<std::uint32_t>(1, (height + 3) / 4);
            const auto blockCount = static_cast<std::size_t>(blocksWide) * blocksHigh;
            const auto payloadOffset = std::size_t{ 128 };
            if (payloadOffset + blockCount * blockBytes > data.size())
                return { 0.34f, 0.48f, 0.22f };

            std::array<double, 3> sum{};
            double samples = 0.0;
            for (std::size_t block = 0; block < blockCount; ++block)
            {
                const auto offset = payloadOffset + block * blockBytes + (isDxt1 ? 0u : 8u);
                const auto c0 = static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
                const auto c1 = static_cast<std::uint16_t>(data[offset + 2] | (data[offset + 3] << 8));
                const auto a = rgb565(c0);
                const auto b = rgb565(c1);
                std::array<std::array<float, 3>, 4> palette{};
                palette[0] = a;
                palette[1] = b;
                if (isDxt1 && c0 <= c1)
                {
                    for (std::size_t i = 0; i < 3; ++i)
                    {
                        palette[2][i] = (a[i] + b[i]) * 0.5f;
                        palette[3][i] = palette[2][i];
                    }
                }
                else
                {
                    for (std::size_t i = 0; i < 3; ++i)
                    {
                        palette[2][i] = (2.0f * a[i] + b[i]) / 3.0f;
                        palette[3][i] = (a[i] + 2.0f * b[i]) / 3.0f;
                    }
                }

                const auto bits = read_le_u32(data, offset + 4);
                for (std::uint32_t pixel = 0; pixel < 16; ++pixel)
                {
                    const auto index = (bits >> (pixel * 2u)) & 0x3u;
                    sum[0] += palette[index][0];
                    sum[1] += palette[index][1];
                    sum[2] += palette[index][2];
                    samples += 1.0;
                }
            }

            if (samples <= 0.0)
                return { 0.34f, 0.48f, 0.22f };
            return {
                static_cast<float>(sum[0] / samples),
                static_cast<float>(sum[1] / samples),
                static_cast<float>(sum[2] / samples),
            };
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
            const auto stem = state_.worldMapPaths[i].filename().string();
            if (stem.size() > 5)
            {
                char* end = nullptr;
                const long n = std::strtol(stem.c_str() + 5, &end, 10);
                if (end != stem.c_str() + 5 && *end == '\0' && n == 1)
                {
                    defaultMap = i;
                    break;
                }
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
        auto validDataRoot = [](const std::filesystem::path& path) {
            return std::filesystem::exists(path / "World")
                || std::filesystem::exists(path / "Assets")
                || std::filesystem::exists(path / "Character");
        };

        std::vector<std::filesystem::path> candidates;
        if (const char* envValue = std::getenv("PHOENIX_ENGINE_DATA"); envValue && envValue[0])
            candidates.emplace_back(envValue);

        candidates.push_back(executableDir / "Data");
        candidates.push_back(std::filesystem::current_path() / "Data");
        candidates.push_back(executableDir.parent_path() / "Data");
        candidates.push_back(executableDir.parent_path().parent_path() / "Data");
        candidates.push_back(executableDir.parent_path().parent_path().parent_path() / "Data");

#ifdef _WIN32
        if (const char* localAppData = std::getenv("LOCALAPPDATA"); localAppData && localAppData[0])
            candidates.emplace_back(std::filesystem::path(localAppData) / "Phoenix Engine" / "Data");
        if (const char* programData = std::getenv("PROGRAMDATA"); programData && programData[0])
            candidates.emplace_back(std::filesystem::path(programData) / "Phoenix Engine" / "Data");
#else
        if (const char* home = std::getenv("HOME"); home && home[0])
            candidates.emplace_back(std::filesystem::path(home) / ".local" / "share" / "Phoenix Engine" / "Data");
#endif

        for (const auto& candidate : candidates)
            if (validDataRoot(candidate))
                return candidate;

        return candidates.empty() ? executableDir / "Data" : candidates.front();
    }

    bool PhoenixRuntime::load_world_map(std::size_t mapIndex)
    {
        if (mapIndex >= state_.worldMapPaths.size())
            return false;

        state_.selectedWorldMap = mapIndex;
        state_.world = phoenix::world::load_phoenix_world(state_.worldMapPaths[mapIndex]);
        load_world_assets();
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
            camera_.speed = 40.0f;
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

        // Scan for worldN/ directories containing map.csv (Phoenix World format).
        for (const auto& entry : std::filesystem::directory_iterator(worldRoot))
        {
            if (!entry.is_directory())
                continue;
            const auto dirName = entry.path().filename().string();
            if (dirName.size() < 6 || dirName.substr(0, 5) != "world")
                continue;
            if (!std::filesystem::exists(entry.path() / "map.csv"))
                continue;
            state_.worldMapPaths.push_back(entry.path());
        }

        std::ranges::sort(state_.worldMapPaths, [](const auto& lhs, const auto& rhs) {
            const auto nameL = lhs.filename().string().substr(5); // strip "world"
            const auto nameR = rhs.filename().string().substr(5);
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

        state_.worldMapNames.reserve(state_.worldMapPaths.size());
        for (const auto& path : state_.worldMapPaths)
            state_.worldMapNames.push_back(path.filename().string());
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

    std::uint32_t PhoenixRuntime::resolve_asset_texture_layer(std::string_view textureName, bool forceCutout)
    {
        // Memoise by name+cutout: the world build asks for the same texture across
        // many meshes, and resolving (3x path lookups + a disk stat) is the bulk of
        // the world-load cost. Same input => same layer, so this is behaviour-exact.
        std::string cacheKey;
        cacheKey.reserve(textureName.size() + 1);
        cacheKey.append(textureName);
        cacheKey.push_back(forceCutout ? '\x01' : '\x00');
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
        const auto cutoutOffset = (forceCutout || static_texture_uses_cutout(textureName)) ? kAssetCutoutLayerBase : 0u;
        if (const auto it = state_.textureSlotByPath.find(key); it != state_.textureSlotByPath.end())
            return cacheResult(kAssetTextureLayerBase + cutoutOffset + it->second);

        if (state_.assetTexturePaths.size() >= kMaxAssetTextureLayers)
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
        const auto assetTextureLayer = [this](std::string_view textureName, bool forceCutout) -> std::uint32_t {
            return resolve_asset_texture_layer(textureName, forceCutout);
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

        // ---- Pass 2: build world assets serially (same order => identical output,
        // including texture-layer slot assignment). ----
        for (auto& p : pending)
        {
            {
                LoadedWorldAsset asset{};
                asset.name = p.name;
                asset.path = p.path;
                const bool isVani = p.key.ends_with(".vani");
                if (!asset.path.empty())
                {
                    const auto assetCutout = static_asset_uses_cutout(p.name, asset.path);
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
                        for (const auto& mesh : model.meshes)
                        {
                            asset.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
                            const auto materialHash = color_hash(mesh.textureName);
                            const auto textureLayer = assetTextureLayer(mesh.textureName, assetCutout);
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
                                if (asset.animationFrames.empty())
                                    asset.animationFrames.resize(asset.frameCount);
                                for (std::uint32_t frame = 0; frame < asset.frameCount; ++frame)
                                {
                                    auto& frameVertices = asset.animationFrames[frame];
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
                        for (const auto& mesh : model.meshes)
                        {
                            asset.vertices += static_cast<std::uint32_t>(mesh.vertices.size());
                            const auto materialHash = color_hash(mesh.textureName);
                            const auto textureLayer = assetTextureLayer(mesh.textureName, assetCutout);
                            const auto base = static_cast<std::uint32_t>(asset.previewVertices.size());
                            asset.previewVertices.reserve(asset.previewVertices.size() + mesh.vertices.size());
                            for (const auto& vertex : mesh.vertices)
                                append_preview_vertex(asset.previewVertices, vertex.position, vertex.normal, vertex.uv, materialHash, textureLayer);
                            asset.previewIndices.reserve(asset.previewIndices.size() + mesh.indices.size());
                            for (const auto index : mesh.indices)
                                asset.previewIndices.push_back(base + index);
                        }
                    }
                }
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
                state_.worldAssets.push_back(std::move(asset));
            }
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
        for (std::size_t sectionIdx = 0; sectionIdx < state_.world.objectSections.size(); ++sectionIdx)
        {
            const auto& section = state_.world.objectSections[sectionIdx];
            for (std::size_t instIdx = 0; instIdx < section.instances.size(); ++instIdx)
            {
                const auto& instance = section.instances[instIdx];
                SceneObject object{};
                object.x = instance.position[0] - halfMap;
                object.y = instance.position[1];
                object.z = instance.position[2] - halfMap;
                std::copy(std::begin(instance.rotationForward), std::end(instance.rotationForward), std::begin(object.forward));
                std::copy(std::begin(instance.rotationUp), std::end(instance.rotationUp), std::begin(object.up));
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

    }

    float PhoenixRuntime::terrain_height(float worldX, float worldZ) const
    {
        if (!state_.world.parsed || state_.world.heightSamples.empty() || state_.world.heightMapSide < 2)
            return 0.0f;

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        const auto halfMap = mapSize * 0.5f;
        const auto u = std::clamp((worldX + halfMap) / mapSize, 0.0f, 1.0f);
        const auto v = std::clamp((worldZ + halfMap) / mapSize, 0.0f, 1.0f);
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
        const auto halfMap = mapSize * 0.5f;
        const auto u = std::clamp((worldX + halfMap) / mapSize, 0.0f, 1.0f);
        const auto v = std::clamp((worldZ + halfMap) / mapSize, 0.0f, 1.0f);
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

    PreviewImage PhoenixRuntime::create_preview_image(std::uint32_t width, std::uint32_t height) const
    {
        PreviewImage image{};
        image.width = std::max(1u, width);
        image.height = std::max(1u, height);
        image.bgra.assign(static_cast<std::size_t>(image.width) * image.height * 4, 255);

        for (std::uint32_t y = 0; y < image.height; ++y)
        {
            for (std::uint32_t x = 0; x < image.width; ++x)
                put_pixel(image, static_cast<int>(x), static_cast<int>(y), 18, 22, 26);
        }

        if (!state_.world.parsed || state_.world.heightSamples.empty() || state_.world.heightMapSide < 2)
            return image;

        const auto side = state_.world.heightMapSide;
        const auto mapSide = std::min(image.width, image.height) - 48u;
        const auto originX = static_cast<int>((image.width - mapSide) / 2u);
        const auto originY = static_cast<int>((image.height - mapSide) / 2u);

        float minHeight = state_.world.heightSamples.front();
        float maxHeight = state_.world.heightSamples.front();
        for (const auto h : state_.world.heightSamples)
        {
            minHeight = std::min(minHeight, h);
            maxHeight = std::max(maxHeight, h);
        }
        const auto heightRange = std::max(1.0f, maxHeight - minHeight);

        for (std::uint32_t y = 0; y < mapSide; ++y)
        {
            const auto sampleY = std::min(side - 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(y) * (side - 1)) / std::max(1u, mapSide - 1)));
            for (std::uint32_t x = 0; x < mapSide; ++x)
            {
                const auto sampleX = std::min(side - 1, static_cast<std::uint32_t>((static_cast<std::uint64_t>(x) * (side - 1)) / std::max(1u, mapSide - 1)));
                const auto h = state_.world.heightSamples[static_cast<std::size_t>(sampleY) * side + sampleX];
                const auto normalized = std::clamp((h - minHeight) / heightRange, 0.0f, 1.0f);
                const auto shade = static_cast<std::uint8_t>(normalized * 95.0f);
                const auto water = normalized < 0.17f;
                const auto r = water ? static_cast<std::uint8_t>(24 + shade / 5) : static_cast<std::uint8_t>(34 + shade);
                const auto g = water ? static_cast<std::uint8_t>(86 + shade / 3) : static_cast<std::uint8_t>(82 + shade);
                const auto b = water ? static_cast<std::uint8_t>(115 + shade / 2) : static_cast<std::uint8_t>(38 + shade / 2);
                put_pixel(image, originX + static_cast<int>(x), originY + static_cast<int>(y), r, g, b);
            }
        }

        for (std::uint32_t i = 0; i <= mapSide; i += std::max(1u, mapSide / 16u))
        {
            for (std::uint32_t p = 0; p < mapSide; ++p)
            {
                put_pixel(image, originX + static_cast<int>(i), originY + static_cast<int>(p), 58, 69, 72);
                put_pixel(image, originX + static_cast<int>(p), originY + static_cast<int>(i), 58, 69, 72);
            }
        }

        const auto mapSize = static_cast<float>(std::max(1u, state_.world.mapSize));
        for (const auto& section : state_.world.objectSections)
        {
            for (const auto& instance : section.instances)
            {
                auto x = instance.position[0] / mapSize;
                auto z = instance.position[2] / mapSize;
                if (x < -0.25f || x > 1.25f || z < -0.25f || z > 1.25f)
                {
                    x = (instance.position[0] + mapSize * 0.5f) / mapSize;
                    z = (instance.position[2] + mapSize * 0.5f) / mapSize;
                }

                const auto px = originX + static_cast<int>(std::clamp(x, 0.0f, 1.0f) * static_cast<float>(mapSide - 1));
                const auto py = originY + static_cast<int>(std::clamp(z, 0.0f, 1.0f) * static_cast<float>(mapSide - 1));
                draw_dot(image, px, py, 2, 238, 179, 58);
            }
        }

        for (int y = originY - 1; y <= originY + static_cast<int>(mapSide); ++y)
        {
            put_pixel(image, originX - 1, y, 210, 218, 214);
            put_pixel(image, originX + static_cast<int>(mapSide), y, 210, 218, 214);
        }
        for (int x = originX - 1; x <= originX + static_cast<int>(mapSide); ++x)
        {
            put_pixel(image, x, originY - 1, 210, 218, 214);
            put_pixel(image, x, originY + static_cast<int>(mapSide), 210, 218, 214);
        }

        return image;
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

    std::vector<std::filesystem::path> PhoenixRuntime::asset_texture_paths() const
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

                    for (int lod = 0; lod < kTerrainLodLevels; ++lod)
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

        {
        }
        return;

        std::vector<std::size_t> objectOrder(state_.sceneObjects.size());
        std::iota(objectOrder.begin(), objectOrder.end(), std::size_t{});
        std::ranges::sort(objectOrder, [this](const auto lhs, const auto rhs) {
            const auto& a = state_.sceneObjects[lhs];
            const auto& b = state_.sceneObjects[rhs];
            if (a.loaded != b.loaded)
                return a.loaded > b.loaded;
            return a.radius > b.radius;
        });
        std::vector<std::uint8_t> meshRendered(state_.sceneObjects.size());
        std::uint32_t highObjectCount{};

        const auto appendTransformedMesh = [this, &vertices, &indices](
            const SceneObject& object,
            const std::vector<phoenix::renderer::TerrainVertex>& sourceVertices,
            const std::vector<std::uint32_t>& sourceIndices) {
            if (sourceVertices.empty() || sourceIndices.empty())
                return false;

            const auto base = static_cast<std::uint32_t>(vertices.size());
            float forward[3]{ object.forward[0], object.forward[1], object.forward[2] };
            float up[3]{ object.up[0], object.up[1], object.up[2] };
            auto normalize = [](float* vector, const float* fallback) {
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
            const float fallbackForward[3]{ 0.0f, 0.0f, 1.0f };
            const float fallbackUp[3]{ 0.0f, 1.0f, 0.0f };
            normalize(forward, fallbackForward);
            normalize(up, fallbackUp);
            const float right[3]{
                up[1] * forward[2] - up[2] * forward[1],
                up[2] * forward[0] - up[0] * forward[2],
                up[0] * forward[1] - up[1] * forward[0],
            };

            const auto requiredVertices = vertices.size() + sourceVertices.size();
            if (vertices.capacity() < requiredVertices)
                vertices.reserve(std::max(requiredVertices, vertices.capacity() * 2u));
            for (const auto& source : sourceVertices)
            {
                phoenix::renderer::TerrainVertex vertex = source;
                const auto lx = source.position[0];
                const auto ly = source.position[1];
                const auto lz = source.position[2];
                vertex.position[0] = object.x + right[0] * lx + up[0] * ly + forward[0] * lz;
                vertex.position[1] = object.y + right[1] * lx + up[1] * ly + forward[1] * lz;
                vertex.position[2] = object.z + right[2] * lx + up[2] * ly + forward[2] * lz;
                vertices.push_back(vertex);
            }

            const auto requiredIndices = indices.size() + sourceIndices.size();
            if (indices.capacity() < requiredIndices)
                indices.reserve(std::max(requiredIndices, indices.capacity() * 2u));
            for (const auto index : sourceIndices)
                indices.push_back(base + index);
            return true;
        };

        for (const auto objectIndex : objectOrder)
        {
            const auto& object = state_.sceneObjects[objectIndex];
            constexpr std::size_t kMaxHighObjectVertices = 5500000;
            const auto useMesh = object.assetSlot >= 0
                && static_cast<std::size_t>(object.assetSlot) < state_.worldAssets.size()
                && !state_.worldAssets[static_cast<std::size_t>(object.assetSlot)].previewVertices.empty()
                && !state_.worldAssets[static_cast<std::size_t>(object.assetSlot)].previewIndices.empty()
                && vertices.size() + state_.worldAssets[static_cast<std::size_t>(object.assetSlot)].previewVertices.size() < kMaxHighObjectVertices;

            if (useMesh)
            {
                const auto& asset = state_.worldAssets[static_cast<std::size_t>(object.assetSlot)];
                appendTransformedMesh(object, asset.previewVertices, asset.previewIndices);
                meshRendered[objectIndex] = 1;
                ++highObjectCount;
                continue;
            }
        }

        std::uint32_t loadedPlaceholders{};
        std::uint32_t missingPlaceholders{};
        for (std::size_t objectIndex = 0; objectIndex < state_.sceneObjects.size(); ++objectIndex)
        {
            if (meshRendered[objectIndex])
                continue;
            const auto& object = state_.sceneObjects[objectIndex];
            if (object.loaded)
                ++loadedPlaceholders;
            else
                ++missingPlaceholders;
            const auto h = object.y + 1.5f;
            const auto size = std::clamp(object.radius * 0.08f, 1.8f, 7.0f);
            const auto base = static_cast<std::uint32_t>(vertices.size());
            const float color[3] = {
                object.loaded ? 0.96f : 0.85f,
                object.loaded ? 0.68f : 0.20f,
                object.loaded ? 0.18f : 0.18f,
            };
            const float points[4][3] = {
                { object.x, h, object.z - size },
                { object.x + size, h, object.z },
                { object.x, h, object.z + size },
                { object.x - size, h, object.z },
            };
            for (const auto& point : points)
            {
                phoenix::renderer::TerrainVertex vertex{};
                vertex.position[0] = point[0];
                vertex.position[1] = point[1];
                vertex.position[2] = point[2];
                vertex.color[0] = color[0];
                vertex.color[1] = color[1];
                vertex.color[2] = color[2];
                vertex.normal[0] = 0.0f;
                vertex.normal[1] = 1.0f;
                vertex.normal[2] = 0.0f;
                vertex.textureLayer = 0xFFFFFFFFu;
                vertices.push_back(vertex);
            }
            indices.push_back(base + 0);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base + 0);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        }

    }

    StaticObjectScene PhoenixRuntime::build_static_object_scene() const
    {
        StaticObjectScene scene;
        if (state_.worldAssets.empty() || state_.sceneObjects.empty())
            return scene;

        constexpr float kCellSize = 512.0f;
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
            ++batchCount; // one merged batch per unique asset
            for (const auto& group : groupsByAsset[assetSlot])
                instanceCount += group.objectIndices.size();
        }

        scene.vertices.reserve(vertexCount);
        scene.indices.reserve(indexCount);
        scene.instances.reserve(instanceCount);
        scene.batches.reserve(batchCount);
        scene.batchBounds.reserve(batchCount);

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

        const auto appendInstance = [&](const SceneObject& object) {
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

            phoenix::renderer::ObjectInstance instance{};
            instance.right[0] = right[0];
            instance.right[1] = right[1];
            instance.right[2] = right[2];
            instance.up[0] = up[0];
            instance.up[1] = up[1];
            instance.up[2] = up[2];
            instance.forward[0] = forward[0];
            instance.forward[1] = forward[1];
            instance.forward[2] = forward[2];
            instance.position[0] = object.x;
            instance.position[1] = object.y;
            instance.position[2] = object.z;
            instance.position[3] = 1.0f;
            scene.instances.push_back(instance);
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

            // Merge all spatial groups of this asset into ONE batch.
            // Instances are contiguous per asset, so a single draw call
            // covers all instances — trading per-cell frustum culling for
            // dramatically fewer draw calls (GPU clips off-screen instances
            // after vertex shading, which is cheap).
            const auto firstInstance = static_cast<std::uint32_t>(scene.instances.size());

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();

            for (const auto& group : groups)
            {
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
            }

            const auto totalInstances = static_cast<std::uint32_t>(scene.instances.size()) - firstInstance;

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
            batch.instanceCount = totalInstances;
            scene.batches.push_back(batch);

            scene.batchBounds.push_back(bounds);
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

        const auto appendInstance = [&](const SceneObject& object) {
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

            phoenix::renderer::ObjectInstance instance{};
            instance.right[0] = right[0];
            instance.right[1] = right[1];
            instance.right[2] = right[2];
            instance.up[0] = up[0];
            instance.up[1] = up[1];
            instance.up[2] = up[2];
            instance.forward[0] = forward[0];
            instance.forward[1] = forward[1];
            instance.forward[2] = forward[2];
            instance.position[0] = object.x;
            instance.position[1] = object.y;
            instance.position[2] = object.z;
            instance.position[3] = 1.0f;
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

            if (asset.vertexAnimated && asset.animationFrames.size() == asset.frameCount)
            {
                AnimatedObjectScene::VertexAnimation animation{};
                animation.firstVertex = baseVertex;
                animation.vertexCount = static_cast<std::uint32_t>(asset.previewVertices.size());
                animation.frames = asset.animationFrames;
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

    void PhoenixRuntime::update_animated_object_scene(AnimatedObjectScene& scene, float totalTime) const
    {
        constexpr float kDecorFps = 12.0f;

        for (auto& animation : scene.vertexAnimations)
        {
            if (animation.frames.empty())
                continue;
            const auto frame = static_cast<std::size_t>(std::floor(totalTime * kDecorFps)) % animation.frames.size();
            const auto& frameVertices = animation.frames[frame];
            const auto count = std::min<std::size_t>(animation.vertexCount, frameVertices.size());
            if (static_cast<std::size_t>(animation.firstVertex) + count <= scene.vertices.size())
            {
                std::copy_n(frameVertices.begin(), count, scene.vertices.begin() + animation.firstVertex);
                scene.mark_vertices_dirty(animation.firstVertex, static_cast<std::uint32_t>(count));
            }
        }


        scene.instances = scene.baseInstances;
        const auto rotateVector = [](float* v, const float* axis, float angle) {
            const float c = std::cos(angle);
            const float s = std::sin(angle);
            const float dot = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
            const float cross[3]{
                axis[1] * v[2] - axis[2] * v[1],
                axis[2] * v[0] - axis[0] * v[2],
                axis[0] * v[1] - axis[1] * v[0],
            };
            v[0] = v[0] * c + cross[0] * s + axis[0] * dot * (1.0f - c);
            v[1] = v[1] * c + cross[1] * s + axis[1] * dot * (1.0f - c);
            v[2] = v[2] * c + cross[2] * s + axis[2] * dot * (1.0f - c);
        };

        for (const auto& animation : scene.instanceAnimations)
        {
            if (animation.instanceIndex >= scene.instances.size())
                continue;
            auto& instance = scene.instances[animation.instanceIndex];
            float axis[3]{
                instance.right[0] * animation.axis[0] + instance.up[0] * animation.axis[1] + instance.forward[0] * animation.axis[2],
                instance.right[1] * animation.axis[0] + instance.up[1] * animation.axis[1] + instance.forward[1] * animation.axis[2],
                instance.right[2] * animation.axis[0] + instance.up[2] * animation.axis[1] + instance.forward[2] * animation.axis[2],
            };
            const auto axisLength = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (axisLength < 0.001f)
                continue;
            axis[0] /= axisLength;
            axis[1] /= axisLength;
            axis[2] /= axisLength;

            const auto angle = totalTime * animation.speed * 6.28318530718f;
            rotateVector(instance.right, axis, angle);
            rotateVector(instance.up, axis, angle);
            rotateVector(instance.forward, axis, angle);
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
            camera_.yaw += input.mouseDx * 0.0032f;
            camera_.pitch = std::clamp(camera_.pitch + input.mouseDy * 0.0032f, -1.25f, 0.45f);
        }
        if (input.yawLeft)
            camera_.yaw -= deltaSeconds * 1.8f;
        if (input.yawRight)
            camera_.yaw += deltaSeconds * 1.8f;
        if (input.pitchUp)
            camera_.pitch = std::clamp(camera_.pitch - deltaSeconds * 1.4f, -1.25f, 0.45f);
        if (input.pitchDown)
            camera_.pitch = std::clamp(camera_.pitch + deltaSeconds * 1.4f, -1.25f, 0.45f);

        const auto dungeonMode = state_.world.isDungeon;
        const auto minSpeed = dungeonMode ? 15.0f : 80.0f;
        const auto maxSpeed = dungeonMode ? 300.0f : 1400.0f;
        camera_.speed = std::clamp(camera_.speed + input.wheel * 0.10f, minSpeed, maxSpeed);
        const auto speed = camera_.speed * (input.fast ? 3.2f : 1.0f) * std::max(0.0f, deltaSeconds);
        const auto cy = std::cos(camera_.yaw);
        const auto sy = std::sin(camera_.yaw);
        const auto forwardX = sy;
        const auto forwardZ = cy;
        const auto rightX = cy;
        const auto rightZ = -sy;

        float moveX = 0.0f;
        float moveY = 0.0f;
        float moveZ = 0.0f;
        if (input.forward)
        {
            moveX += forwardX;
            moveZ += forwardZ;
        }
        if (input.backward)
        {
            moveX -= forwardX;
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

        const auto horizontalLength = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (horizontalLength > 0.001f)
        {
            moveX /= horizontalLength;
            moveZ /= horizontalLength;
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
