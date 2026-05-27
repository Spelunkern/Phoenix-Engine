#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace phoenix::world
{
    struct MonsterSDataRecord
    {
        std::string name;
        std::int16_t modelId{};
        std::uint8_t size{ 10 };
    };

    struct NpcSDataRecord
    {
        std::string name;
        std::int32_t modelId{};
    };

    struct MonsterSData
    {
        std::unordered_map<std::uint32_t, MonsterSDataRecord> records;
        bool parsed{};
    };

    struct NpcQuestSData
    {
        std::unordered_map<std::uint32_t, NpcSDataRecord> records;
        bool parsed{};
    };

    MonsterSData load_monster_sdata(const std::filesystem::path& path);
    NpcQuestSData load_npcquest_sdata(const std::filesystem::path& path);
}
