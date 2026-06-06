#include "world/phoenix_world_loader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>

namespace phoenix::world
{
    namespace
    {
        // Minimal CSV line parser (handles quoted fields).
        std::vector<std::string> split_csv_line(const std::string& line)
        {
            std::vector<std::string> fields;
            std::string field;
            bool inQuotes = false;
            for (std::size_t i = 0; i < line.size(); ++i)
            {
                const char c = line[i];
                if (c == '"')
                    inQuotes = !inQuotes;
                else if (c == ',' && !inQuotes)
                {
                    fields.push_back(field);
                    field.clear();
                }
                else
                    field += c;
            }
            fields.push_back(field);
            return fields;
        }

        struct CsvReader
        {
            std::vector<std::string> headers;
            std::vector<std::vector<std::string>> rows;

            bool load(const std::filesystem::path& path)
            {
                std::ifstream file(path);
                if (!file) return false;
                std::string line;
                if (!std::getline(file, line)) return false;
                // Strip BOM if present.
                if (line.size() >= 3 && line[0] == '\xEF' && line[1] == '\xBB' && line[2] == '\xBF')
                    line = line.substr(3);
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                    line.pop_back();
                headers = split_csv_line(line);
                while (std::getline(file, line))
                {
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                        line.pop_back();
                    if (line.empty()) continue;
                    rows.push_back(split_csv_line(line));
                }
                return true;
            }

            int col(const std::string& name) const
            {
                for (int i = 0; i < static_cast<int>(headers.size()); ++i)
                    if (headers[i] == name) return i;
                return -1;
            }

            const std::string& get(const std::vector<std::string>& row, int idx) const
            {
                static const std::string empty;
                if (idx < 0 || idx >= static_cast<int>(row.size())) return empty;
                return row[idx];
            }

            float getf(const std::vector<std::string>& row, int idx) const
            {
                auto s = get(row, idx);
                if (s.empty()) return 0.0f;
                for (auto& c : s)
                    if (c == ',') c = '.';
                try { return std::stof(s); }
                catch (...) { return 0.0f; }
            }

            int geti(const std::vector<std::string>& row, int idx) const
            {
                const auto& s = get(row, idx);
                if (s.empty()) return 0;
                try { return std::stoi(s); }
                catch (...) { return 0; }
            }
        };
    }

    bool phoenix_world_exists(const std::filesystem::path& worldsRoot, const std::string& mapId)
    {
        return std::filesystem::exists(worldsRoot / ("world" + mapId) / "map.csv");
    }

    WldAnalysis load_phoenix_world(const std::filesystem::path& worldDir)
    {
        WldAnalysis result{};
        result.path = worldDir;

        // ---- map.csv ----
        {
            CsvReader csv;
            if (!csv.load(worldDir / "map.csv"))
            {
                return result;
            }
            if (csv.rows.empty()) return result;
            const auto& row = csv.rows[0];
            result.mapSize = static_cast<std::uint32_t>(csv.geti(row, csv.col("mapSize")));
            result.isDungeon = csv.geti(row, csv.col("isDungeon")) != 0;
            const auto dgFile = csv.get(row, csv.col("dgFile"));
            if (!dgFile.empty())
                result.dungeonDgFileName = dgFile;
        }

        // ---- heightmap.raw ----
        {
            const auto path = worldDir / "heightmap.raw";
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (file)
            {
                const auto size = file.tellg();
                file.seekg(0);
                const auto sampleCount = static_cast<std::size_t>(size) / 2;
                result.heightMapSide = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(sampleCount)));
                result.heightSamples.resize(sampleCount);
                std::vector<std::uint16_t> raw(sampleCount);
                file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sampleCount * 2));
                result.minInitialHeight = 65535.0f;
                result.maxInitialHeight = 0.0f;
                for (std::size_t i = 0; i < sampleCount; ++i)
                {
                    const auto v = static_cast<float>(raw[i]);
                    result.heightSamples[i] = v;
                    result.minInitialHeight = std::min(result.minInitialHeight, v);
                    result.maxInitialHeight = std::max(result.maxInitialHeight, v);
                }
                if (!result.heightSamples.empty())
                    result.firstHeight = result.heightSamples[0];
            }
        }

        // ---- texturemap.raw ----
        {
            const auto path = worldDir / "texturemap.raw";
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (file)
            {
                const auto size = static_cast<std::size_t>(file.tellg());
                file.seekg(0);
                result.terrainTextureMap.resize(size);
                file.read(reinterpret_cast<char*>(result.terrainTextureMap.data()),
                    static_cast<std::streamsize>(size));
            }
        }

        // ---- terrain_layers.csv ----
        {
            CsvReader csv;
            if (csv.load(worldDir / "terrain_layers.csv"))
            {
                const auto iTexture = csv.col("textureFile");
                const auto iTile = csv.col("tileSize");
                const auto iSound = csv.col("walkSound");
                for (const auto& row : csv.rows)
                {
                    WldTerrainLayer layer;
                    layer.textureFileName = csv.get(row, iTexture);
                    layer.tileSize = csv.getf(row, iTile);
                    layer.walkSoundFileName = csv.get(row, iSound);
                    result.terrainLayers.push_back(std::move(layer));
                }
            }
        }

        // ---- sky.csv ----
        {
            CsvReader csv;
            if (csv.load(worldDir / "sky.csv") && !csv.rows.empty())
            {
                const auto& row = csv.rows[0];
                result.skyFileName = csv.get(row, csv.col("skyTexture"));
                result.primaryCloudFileName = csv.get(row, csv.col("primaryCloud"));
                result.secondaryCloudFileName = csv.get(row, csv.col("secondaryCloud"));
                result.fogColor[0] = csv.getf(row, csv.col("fogR"));
                result.fogColor[1] = csv.getf(row, csv.col("fogG"));
                result.fogColor[2] = csv.getf(row, csv.col("fogB"));
                result.fogStartDistance = csv.getf(row, csv.col("fogStart"));
                result.fogEndDistance = csv.getf(row, csv.col("fogEnd"));
                result.parsedSky = true;
            }
        }

        // ---- objects.csv ----
        {
            CsvReader csv;
            if (csv.load(worldDir / "objects.csv"))
            {
                const auto iCat = csv.col("category");
                const auto iAsset = csv.col("assetFile");
                const auto iPx = csv.col("posX");
                const auto iPy = csv.col("posY");
                const auto iPz = csv.col("posZ");
                const auto iFx = csv.col("fwdX");
                const auto iFy = csv.col("fwdY");
                const auto iFz = csv.col("fwdZ");
                const auto iUx = csv.col("upX");
                const auto iUy = csv.col("upY");
                const auto iUz = csv.col("upZ");

                // Group by category into WldObjectSections.
                std::unordered_map<std::string, std::size_t> sectionMap;
                for (const auto& row : csv.rows)
                {
                    const auto cat = csv.get(row, iCat);
                    const auto asset = csv.get(row, iAsset);
                    if (asset.empty()) continue;

                    auto it = sectionMap.find(cat);
                    if (it == sectionMap.end())
                    {
                        sectionMap[cat] = result.objectSections.size();
                        WldObjectSection sec;
                        sec.name = cat;
                        result.objectSections.push_back(std::move(sec));
                        it = sectionMap.find(cat);
                    }
                    auto& section = result.objectSections[it->second];

                    // Find or add asset index.
                    auto assetIt = std::find(section.assets.begin(), section.assets.end(), asset);
                    std::int32_t assetIdx;
                    if (assetIt != section.assets.end())
                        assetIdx = static_cast<std::int32_t>(assetIt - section.assets.begin());
                    else
                    {
                        assetIdx = static_cast<std::int32_t>(section.assets.size());
                        section.assets.push_back(asset);
                    }

                    WldObjectInstance inst{};
                    inst.assetIndex = assetIdx;
                    inst.position[0] = csv.getf(row, iPx);
                    inst.position[1] = csv.getf(row, iPy);
                    inst.position[2] = csv.getf(row, iPz);
                    inst.rotationForward[0] = csv.getf(row, iFx);
                    inst.rotationForward[1] = csv.getf(row, iFy);
                    inst.rotationForward[2] = csv.getf(row, iFz);
                    inst.rotationUp[0] = csv.getf(row, iUx);
                    inst.rotationUp[1] = csv.getf(row, iUy);
                    inst.rotationUp[2] = csv.getf(row, iUz);
                    section.instances.push_back(inst);
                }
            }
        }

        // ---- portals.csv ----
        {
            CsvReader csv;
            if (csv.load(worldDir / "portals.csv"))
            {
                const auto iMap = csv.col("destMapId");
                const auto iDx = csv.col("destX");
                const auto iDy = csv.col("destY");
                const auto iDz = csv.col("destZ");
                const auto iMinX = csv.col("minX");
                const auto iMinY = csv.col("minY");
                const auto iMinZ = csv.col("minZ");
                const auto iMaxX = csv.col("maxX");
                const auto iMaxY = csv.col("maxY");
                const auto iMaxZ = csv.col("maxZ");
                for (const auto& row : csv.rows)
                {
                    WldPortal portal{};
                    portal.mapId = static_cast<std::uint8_t>(csv.geti(row, iMap));
                    portal.destinationPosition[0] = csv.getf(row, iDx);
                    portal.destinationPosition[1] = csv.getf(row, iDy);
                    portal.destinationPosition[2] = csv.getf(row, iDz);
                    portal.box.min[0] = csv.getf(row, iMinX);
                    portal.box.min[1] = csv.getf(row, iMinY);
                    portal.box.min[2] = csv.getf(row, iMinZ);
                    portal.box.max[0] = csv.getf(row, iMaxX);
                    portal.box.max[1] = csv.getf(row, iMaxY);
                    portal.box.max[2] = csv.getf(row, iMaxZ);
                    result.portals.push_back(std::move(portal));
                }
            }
        }

        // ---- audio.csv ----
        {
            CsvReader csv;
            if (csv.load(worldDir / "audio.csv"))
            {
                const auto iType = csv.col("type");
                const auto iAsset = csv.col("assetFile");
                const auto iPx = csv.col("posX");
                const auto iPy = csv.col("posY");
                const auto iPz = csv.col("posZ");
                const auto iRadius = csv.col("radius");
                const auto iMinX = csv.col("minX");
                const auto iMinY = csv.col("minY");
                const auto iMinZ = csv.col("minZ");
                const auto iMaxX = csv.col("maxX");
                const auto iMaxY = csv.col("maxY");
                const auto iMaxZ = csv.col("maxZ");

                for (const auto& row : csv.rows)
                {
                    const auto type = csv.get(row, iType);
                    const auto asset = csv.get(row, iAsset);
                    if (type == "music")
                    {
                        auto it = std::find(result.musicAssets.begin(), result.musicAssets.end(), asset);
                        std::int32_t idx;
                        if (it != result.musicAssets.end())
                            idx = static_cast<std::int32_t>(it - result.musicAssets.begin());
                        else
                        {
                            idx = static_cast<std::int32_t>(result.musicAssets.size());
                            result.musicAssets.push_back(asset);
                        }
                        WldMusicZone zone{};
                        zone.musicAssetId = idx;
                        zone.box.min[0] = csv.getf(row, iMinX);
                        zone.box.min[1] = csv.getf(row, iMinY);
                        zone.box.min[2] = csv.getf(row, iMinZ);
                        zone.box.max[0] = csv.getf(row, iMaxX);
                        zone.box.max[1] = csv.getf(row, iMaxY);
                        zone.box.max[2] = csv.getf(row, iMaxZ);
                        result.musicZones.push_back(std::move(zone));
                    }
                    else if (type == "sound")
                    {
                        auto it = std::find(result.soundEffectAssets.begin(), result.soundEffectAssets.end(), asset);
                        std::int32_t idx;
                        if (it != result.soundEffectAssets.end())
                            idx = static_cast<std::int32_t>(it - result.soundEffectAssets.begin());
                        else
                        {
                            idx = static_cast<std::int32_t>(result.soundEffectAssets.size());
                            result.soundEffectAssets.push_back(asset);
                        }
                        WldSoundEffect sfx{};
                        sfx.soundEffectAssetId = idx;
                        sfx.center[0] = csv.getf(row, iPx);
                        sfx.center[1] = csv.getf(row, iPy);
                        sfx.center[2] = csv.getf(row, iPz);
                        sfx.radius = csv.getf(row, iRadius);
                        result.soundEffects.push_back(std::move(sfx));
                    }
                }
            }
        }

        // Set paths for self-contained world resources.
        result.phoenixWorldDir = worldDir;
        {
            const auto fieldDir = worldDir / "field";
            if (std::filesystem::is_directory(fieldDir))
                result.phoenixWorldFieldDir = fieldDir;
        }

        result.parsed = true;

        return result;
    }
}
