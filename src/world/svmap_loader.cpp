#include "world/svmap_loader.h"

#include <bit>
#include <fstream>
#include <iterator>

namespace phoenix::world
{
    namespace
    {
        std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 4 > data.size())
                return 0;
            return static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

        std::int32_t read_i32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<std::int32_t>(read_u32(data, offset));
        }

        float read_f32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<float>(read_u32(data, offset));
        }

        bool skip_bytes(std::size_t& offset, std::size_t count, std::size_t size)
        {
            if (count > size - offset)
                return false;
            offset += count;
            return true;
        }

        bool read_vec3(const std::vector<std::uint8_t>& data, std::size_t& offset, SvmapVec3& value)
        {
            if (offset + 12 > data.size())
                return false;
            value.x = read_f32(data, offset + 0);
            value.y = read_f32(data, offset + 4);
            value.z = read_f32(data, offset + 8);
            offset += 12;
            return true;
        }

        bool read_box(const std::vector<std::uint8_t>& data, std::size_t& offset, SvmapBox& box)
        {
            return read_vec3(data, offset, box.min) && read_vec3(data, offset, box.max);
        }

        bool read_count(const std::vector<std::uint8_t>& data, std::size_t& offset, std::uint32_t& count, std::uint32_t cap)
        {
            if (offset + 4 > data.size())
                return false;
            count = read_u32(data, offset);
            offset += 4;
            return count <= cap;
        }
    }

    SvmapFile load_svmap(const std::filesystem::path& path)
    {
        SvmapFile svmap{};
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return svmap;

        std::vector<std::uint8_t> data{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>() };
        if (data.size() < 12)
            return svmap;

        std::size_t offset = 0;
        svmap.mapSize = read_i32(data, offset);
        offset += 4;
        if (svmap.mapSize <= 0 || svmap.mapSize > 1'000'000)
            return {};

        const auto maskBytes = static_cast<std::size_t>(svmap.mapSize) * static_cast<std::size_t>(svmap.mapSize) / 8u;
        if (!skip_bytes(offset, maskBytes, data.size()) || offset + 4 > data.size())
            return {};

        svmap.cellSize = read_i32(data, offset);
        offset += 4;

        std::uint32_t count{};
        if (!read_count(data, offset, count, 200000) || !skip_bytes(offset, static_cast<std::size_t>(count) * 12u, data.size()))
            return {};

        if (!read_count(data, offset, count, 200000))
            return {};
        svmap.monsterAreas.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            SvmapMonsterArea area{};
            if (!read_box(data, offset, area.area))
                return {};
            std::uint32_t spawnCount{};
            if (!read_count(data, offset, spawnCount, 4096))
                return {};
            area.spawns.reserve(spawnCount);
            for (std::uint32_t s = 0; s < spawnCount; ++s)
            {
                if (offset + 8 > data.size())
                    return {};
                SvmapMonsterSpawn spawn{};
                spawn.mobId = read_u32(data, offset + 0);
                spawn.count = read_u32(data, offset + 4);
                offset += 8;
                area.spawns.push_back(spawn);
            }
            svmap.monsterAreas.push_back(std::move(area));
        }

        if (!read_count(data, offset, count, 200000))
            return {};
        svmap.npcs.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            if (offset + 8 > data.size())
                return {};
            SvmapNpc npc{};
            npc.npcType = read_i32(data, offset + 0);
            npc.npcId = read_i32(data, offset + 4);
            offset += 8;
            std::uint32_t positionCount{};
            if (!read_count(data, offset, positionCount, 4096))
                return {};
            npc.positions.reserve(positionCount);
            for (std::uint32_t p = 0; p < positionCount; ++p)
            {
                SvmapNpcPosition position{};
                if (!read_vec3(data, offset, position.position) || offset + 4 > data.size())
                    return {};
                position.yaw = read_f32(data, offset);
                offset += 4;
                npc.positions.push_back(position);
            }
            svmap.npcs.push_back(std::move(npc));
        }

        svmap.parsed = true;
        return svmap;
    }
}
