#include "world/svmap_loader.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::world
{
    namespace
    {
        std::string next_field(const std::string& line, std::size_t& pos)
        {
            auto comma = line.find(',', pos);
            if (comma == std::string::npos) comma = line.size();
            auto token = line.substr(pos, comma - pos);
            pos = comma < line.size() ? comma + 1 : line.size();
            return token;
        }
    }

    SvmapFile load_svmap(const std::filesystem::path& path)
    {
        SvmapFile svmap{};

        // Resolve CSV folder: Data/World/svmap/{mapId}/
        // path is e.g. Data/World/1.svmap — extract stem as map ID.
        const auto mapId = path.stem().string();
        const auto csvDir = path.parent_path() / "svmap" / mapId;
        if (!std::filesystem::exists(csvDir))
            return svmap;

        // metadata.csv: MapFile,MapSize,MapMaskBytes,CellSize
        {
            std::ifstream file(csvDir / "metadata.csv");
            if (!file) return svmap;
            std::string line;
            std::getline(file, line); // header
            if (!std::getline(file, line)) return svmap;
            std::size_t pos = 0;
            next_field(line, pos); // MapFile
            svmap.mapSize = std::atoi(next_field(line, pos).c_str());
            next_field(line, pos); // MapMaskBytes
            svmap.cellSize = std::atoi(next_field(line, pos).c_str());
        }
        if (svmap.mapSize <= 0)
            return svmap;

        // monster_areas.csv: AreaIndex,LowerX,LowerY,LowerZ,UpperX,UpperY,UpperZ,MonsterCount
        struct AreaEntry { SvmapBox box{}; };
        std::unordered_map<int, AreaEntry> areaEntries;
        {
            std::ifstream file(csvDir / "monster_areas.csv");
            if (file)
            {
                std::string line;
                std::getline(file, line);
                while (std::getline(file, line))
                {
                    if (line.empty()) continue;
                    std::size_t pos = 0;
                    auto areaIdx = std::atoi(next_field(line, pos).c_str());
                    AreaEntry entry{};
                    entry.box.min.x = std::strtof(next_field(line, pos).c_str(), nullptr);
                    entry.box.min.y = std::strtof(next_field(line, pos).c_str(), nullptr);
                    entry.box.min.z = std::strtof(next_field(line, pos).c_str(), nullptr);
                    entry.box.max.x = std::strtof(next_field(line, pos).c_str(), nullptr);
                    entry.box.max.y = std::strtof(next_field(line, pos).c_str(), nullptr);
                    entry.box.max.z = std::strtof(next_field(line, pos).c_str(), nullptr);
                    areaEntries[areaIdx] = entry;
                }
            }
        }

        // monster_spawns.csv: AreaIndex,MonsterIndex,MobId,Count
        std::unordered_map<int, std::vector<SvmapMonsterSpawn>> spawnsByArea;
        {
            std::ifstream file(csvDir / "monster_spawns.csv");
            if (file)
            {
                std::string line;
                std::getline(file, line);
                while (std::getline(file, line))
                {
                    if (line.empty()) continue;
                    std::size_t pos = 0;
                    auto areaIdx = std::atoi(next_field(line, pos).c_str());
                    next_field(line, pos); // MonsterIndex
                    SvmapMonsterSpawn spawn{};
                    spawn.mobId = static_cast<std::uint32_t>(std::atoi(next_field(line, pos).c_str()));
                    spawn.count = static_cast<std::uint32_t>(std::atoi(next_field(line, pos).c_str()));
                    spawnsByArea[areaIdx].push_back(spawn);
                }
            }
        }

        for (auto& [areaIdx, entry] : areaEntries)
        {
            SvmapMonsterArea area{};
            area.area = entry.box;
            if (auto it = spawnsByArea.find(areaIdx); it != spawnsByArea.end())
                area.spawns = std::move(it->second);
            svmap.monsterAreas.push_back(std::move(area));
        }

        // npcs.csv: NpcGroupIndex,NpcType,NpcId,PositionCount
        // npc_positions.csv: NpcGroupIndex,PositionIndex,X,Y,Z,Yaw
        struct NpcEntry { std::int32_t npcType{}; std::int32_t npcId{}; };
        std::unordered_map<int, NpcEntry> npcEntries;
        {
            std::ifstream file(csvDir / "npcs.csv");
            if (file)
            {
                std::string line;
                std::getline(file, line);
                while (std::getline(file, line))
                {
                    if (line.empty()) continue;
                    std::size_t pos = 0;
                    auto groupIdx = std::atoi(next_field(line, pos).c_str());
                    NpcEntry entry{};
                    entry.npcType = std::atoi(next_field(line, pos).c_str());
                    entry.npcId = std::atoi(next_field(line, pos).c_str());
                    npcEntries[groupIdx] = entry;
                }
            }
        }

        std::unordered_map<int, std::vector<SvmapNpcPosition>> npcPositions;
        {
            std::ifstream file(csvDir / "npc_positions.csv");
            if (file)
            {
                std::string line;
                std::getline(file, line);
                while (std::getline(file, line))
                {
                    if (line.empty()) continue;
                    std::size_t pos = 0;
                    auto groupIdx = std::atoi(next_field(line, pos).c_str());
                    next_field(line, pos); // PositionIndex
                    SvmapNpcPosition position{};
                    position.position.x = std::strtof(next_field(line, pos).c_str(), nullptr);
                    position.position.y = std::strtof(next_field(line, pos).c_str(), nullptr);
                    position.position.z = std::strtof(next_field(line, pos).c_str(), nullptr);
                    position.yaw = std::strtof(next_field(line, pos).c_str(), nullptr);
                    npcPositions[groupIdx].push_back(position);
                }
            }
        }

        for (auto& [groupIdx, entry] : npcEntries)
        {
            SvmapNpc npc{};
            npc.npcType = entry.npcType;
            npc.npcId = entry.npcId;
            if (auto it = npcPositions.find(groupIdx); it != npcPositions.end())
                npc.positions = std::move(it->second);
            svmap.npcs.push_back(std::move(npc));
        }

        svmap.parsed = true;
        return svmap;
    }
}
