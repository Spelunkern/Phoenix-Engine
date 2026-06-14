#include "world/svmap_loader.h"

#include "assets/data_index.h"

#include <cstring>
#include <vector>

namespace phoenix::world
{
    namespace
    {
        // Sequential little-endian reader over an in-memory buffer. Every read is
        // bounds-checked; once `ok` flips false all further reads are no-ops and
        // the caller bails out, so a truncated/corrupt file can never overrun.
        struct Cursor
        {
            const std::uint8_t* data{};
            std::size_t size{};
            std::size_t pos{};
            bool ok{ true };

            bool ensure(std::size_t bytes)
            {
                if (!ok || pos + bytes > size)
                {
                    ok = false;
                    return false;
                }
                return true;
            }

            std::int32_t read_i32()
            {
                if (!ensure(4))
                    return 0;
                std::int32_t v{};
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                return v;
            }

            float read_f32()
            {
                if (!ensure(4))
                    return 0.0f;
                float v{};
                std::memcpy(&v, data + pos, 4);
                pos += 4;
                return v;
            }

            // Advance over `count` items of `stride` bytes without copying.
            void skip(std::size_t bytes)
            {
                if (ensure(bytes))
                    pos += bytes;
            }
        };
    }

    SvmapNpcData load_svmap_npcs(const std::filesystem::path& path)
    {
        SvmapNpcData result{};

        const std::vector<std::uint8_t> buffer = phoenix::assets::read_file_binary(path);
        if (buffer.empty())
            return result;

        Cursor cursor{ buffer.data(), buffer.size(), 0, true };

        const std::int32_t mapSize = cursor.read_i32();
        if (!cursor.ok || mapSize <= 0)
            return result;
        result.mapSize = static_cast<std::uint32_t>(mapSize);

        // 1-bit accessibility mask: (mapSize * mapSize / 8) bytes.
        const std::size_t maskBytes =
            static_cast<std::size_t>(mapSize) * static_cast<std::size_t>(mapSize) / 8u;
        cursor.skip(maskBytes);

        cursor.read_i32(); // CellSize (unused here)

        // Ladders: length-prefixed list of Vector3 (12 bytes each).
        const std::int32_t ladderCount = cursor.read_i32();
        if (ladderCount > 0)
            cursor.skip(static_cast<std::size_t>(ladderCount) * 12u);

        // Monster spawn areas: each is a BoundingBox (24 bytes) followed by a
        // length-prefixed list of monster spawns (8 bytes each).
        const std::int32_t monsterAreaCount = cursor.read_i32();
        for (std::int32_t i = 0; i < monsterAreaCount && cursor.ok; ++i)
        {
            cursor.skip(24); // BoundingBox
            const std::int32_t spawnCount = cursor.read_i32();
            if (spawnCount > 0)
                cursor.skip(static_cast<std::size_t>(spawnCount) * 8u);
        }
        if (!cursor.ok)
            return result;

        // NPCs: length-prefixed list. Each entry is { int32 type, int32 id,
        // list<position> }, where each position is { Vector3, float yaw }.
        const std::int32_t npcGroupCount = cursor.read_i32();
        if (!cursor.ok || npcGroupCount < 0)
            return result;

        result.groups.reserve(static_cast<std::size_t>(npcGroupCount));
        for (std::int32_t g = 0; g < npcGroupCount && cursor.ok; ++g)
        {
            SvmapNpcGroup group{};
            group.npcType = cursor.read_i32();
            group.npcId = cursor.read_i32();
            const std::int32_t positionCount = cursor.read_i32();
            if (!cursor.ok || positionCount < 0)
                return result;

            group.waypoints.reserve(static_cast<std::size_t>(positionCount));
            for (std::int32_t p = 0; p < positionCount && cursor.ok; ++p)
            {
                SvmapNpcWaypoint wp{};
                wp.x = cursor.read_f32();
                wp.y = cursor.read_f32();
                wp.z = cursor.read_f32();
                wp.yaw = cursor.read_f32();
                if (!cursor.ok)
                    return result;
                group.waypoints.push_back(wp);
            }
            if (!group.waypoints.empty())
                result.groups.push_back(std::move(group));
        }

        result.parsed = cursor.ok;
        return result;
    }

    SvmapMonsterData load_svmap_monsters(const std::filesystem::path& path)
    {
        SvmapMonsterData result{};

        const std::vector<std::uint8_t> buffer = phoenix::assets::read_file_binary(path);
        if (buffer.empty())
            return result;

        Cursor cursor{ buffer.data(), buffer.size(), 0, true };

        const std::int32_t mapSize = cursor.read_i32();
        if (!cursor.ok || mapSize <= 0)
            return result;
        result.mapSize = static_cast<std::uint32_t>(mapSize);

        const std::size_t maskBytes =
            static_cast<std::size_t>(mapSize) * static_cast<std::size_t>(mapSize) / 8u;
        cursor.skip(maskBytes);

        cursor.read_i32(); // CellSize (unused here)

        // Ladders: length-prefixed list of Vector3 (12 bytes each).
        const std::int32_t ladderCount = cursor.read_i32();
        if (ladderCount > 0)
            cursor.skip(static_cast<std::size_t>(ladderCount) * 12u);

        // Monster spawn areas: each is a BoundingBox (LowerLimit + UpperLimit,
        // 6 floats) followed by a length-prefixed list of { uint mobId, uint count }.
        const std::int32_t areaCount = cursor.read_i32();
        if (!cursor.ok || areaCount < 0)
            return result;

        result.areas.reserve(static_cast<std::size_t>(areaCount));
        for (std::int32_t i = 0; i < areaCount && cursor.ok; ++i)
        {
            SvmapMonsterArea area{};
            area.minX = cursor.read_f32();
            area.minY = cursor.read_f32();
            area.minZ = cursor.read_f32();
            area.maxX = cursor.read_f32();
            area.maxY = cursor.read_f32();
            area.maxZ = cursor.read_f32();
            const std::int32_t spawnCount = cursor.read_i32();
            if (!cursor.ok || spawnCount < 0)
                return result;

            area.mobs.reserve(static_cast<std::size_t>(spawnCount));
            for (std::int32_t s = 0; s < spawnCount && cursor.ok; ++s)
            {
                SvmapMonsterSpawn mob{};
                mob.mobId = static_cast<std::uint32_t>(cursor.read_i32());
                mob.count = static_cast<std::uint32_t>(cursor.read_i32());
                if (!cursor.ok)
                    return result;
                area.mobs.push_back(mob);
            }
            if (!area.mobs.empty())
                result.areas.push_back(std::move(area));
        }

        result.parsed = cursor.ok;
        return result;
    }
}
