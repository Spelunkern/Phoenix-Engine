#include "world/sdata_loader.h"

#include "assets/data_index.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace phoenix::world
{
    MonsterSData load_monster_sdata(const std::filesystem::path& path)
    {
        MonsterSData out{};
        auto file = assets::open_ifstream(path);
        if (!file)
            return out;
        std::string line;
        std::getline(file, line); // skip header
        // RecordIndex,MobName,ModelId,Level,Ai,Hp,Day,Size,...
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::size_t pos = 0;
            auto next = [&]() -> std::string {
                auto comma = line.find(',', pos);
                if (comma == std::string::npos) comma = line.size();
                auto token = line.substr(pos, comma - pos);
                pos = comma < line.size() ? comma + 1 : line.size();
                return token;
            };
            const auto id = static_cast<std::uint32_t>(std::atoi(next().c_str()));
            MonsterSDataRecord record{};
            record.name = next();
            record.modelId = static_cast<std::int16_t>(std::atoi(next().c_str()));
            next(); // Level
            next(); // Ai
            next(); // Hp
            next(); // Day
            record.size = static_cast<std::uint8_t>(std::atoi(next().c_str()));
            out.records[id] = std::move(record);
        }
        out.parsed = !out.records.empty();
        return out;
    }

    NpcQuestSData load_npcquest_sdata(const std::filesystem::path& path)
    {
        NpcQuestSData out{};
        auto file = assets::open_ifstream(path);
        if (!file)
            return out;
        std::string line;
        std::getline(file, line); // skip header
        // Category,NpcType,NpcTypeId,Model,Name
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::size_t pos = 0;
            auto next = [&]() -> std::string {
                auto comma = line.find(',', pos);
                if (comma == std::string::npos) comma = line.size();
                auto token = line.substr(pos, comma - pos);
                pos = comma < line.size() ? comma + 1 : line.size();
                return token;
            };
            next(); // category
            const auto npcType = std::atoi(next().c_str());
            const auto npcTypeId = std::atoi(next().c_str());
            const auto modelId = std::atoi(next().c_str());
            auto name = next();
            const auto key = (static_cast<std::uint32_t>(npcType) << 16)
                | static_cast<std::uint16_t>(npcTypeId);
            NpcSDataRecord record{};
            record.modelId = modelId;
            record.name = std::move(name);
            out.records[key] = std::move(record);
        }
        out.parsed = !out.records.empty();
        return out;
    }
}
