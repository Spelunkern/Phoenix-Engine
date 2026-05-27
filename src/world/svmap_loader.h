#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace phoenix::world
{
    struct SvmapVec3
    {
        float x{};
        float y{};
        float z{};
    };

    struct SvmapBox
    {
        SvmapVec3 min{};
        SvmapVec3 max{};
    };

    struct SvmapMonsterSpawn
    {
        std::uint32_t mobId{};
        std::uint32_t count{};
    };

    struct SvmapMonsterArea
    {
        SvmapBox area{};
        std::vector<SvmapMonsterSpawn> spawns;
    };

    struct SvmapNpcPosition
    {
        SvmapVec3 position{};
        float yaw{};
    };

    struct SvmapNpc
    {
        std::int32_t npcType{};
        std::int32_t npcId{};
        std::vector<SvmapNpcPosition> positions;
    };

    struct SvmapFile
    {
        std::int32_t mapSize{};
        std::int32_t cellSize{};
        std::vector<SvmapMonsterArea> monsterAreas;
        std::vector<SvmapNpc> npcs;
        bool parsed{};
    };

    SvmapFile load_svmap(const std::filesystem::path& path);
}
