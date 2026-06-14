#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace phoenix::world
{
    // A single authored position from a map's .svmap file. Coordinates are in
    // raw Shaiya map space (the same space as WLD instance positions, i.e.
    // [0, mapSize]); the caller converts to engine/world space.
    struct SvmapNpcWaypoint
    {
        float x{};
        float y{};
        float z{};
        float yaw{};
    };

    // One NPC entry. A single waypoint is a static NPC; multiple waypoints are a
    // patrol path the NPC walks between (it is one NPC, not one per position).
    struct SvmapNpcGroup
    {
        std::int32_t npcType{};
        std::int32_t npcId{};
        std::vector<SvmapNpcWaypoint> waypoints;
    };

    struct SvmapNpcData
    {
        bool parsed{};
        std::uint32_t mapSize{};
        std::vector<SvmapNpcGroup> groups;
    };

    // Parses only the NPC entries out of a .svmap file. The other sections
    // (map mask, ladders, monster areas, portals, spawns, named areas) are
    // walked past but not retained. Returns parsed=false on any read error.
    SvmapNpcData load_svmap_npcs(const std::filesystem::path& path);

    // One mob kind that spawns in an area, and how many of it.
    struct SvmapMonsterSpawn
    {
        std::uint32_t mobId{};
        std::uint32_t count{};
    };

    // A 2-D-ish spawn region (a flat box) with the mobs that populate it. The box
    // is in raw Shaiya map space; the caller converts to engine/world space.
    struct SvmapMonsterArea
    {
        float minX{};
        float minY{};
        float minZ{};
        float maxX{};
        float maxY{};
        float maxZ{};
        std::vector<SvmapMonsterSpawn> mobs;
    };

    struct SvmapMonsterData
    {
        bool parsed{};
        std::uint32_t mapSize{};
        std::vector<SvmapMonsterArea> areas;
    };

    // Parses only the monster spawn areas (which precede the NPC list in the
    // file). Returns parsed=false on any read error.
    SvmapMonsterData load_svmap_monsters(const std::filesystem::path& path);
}
