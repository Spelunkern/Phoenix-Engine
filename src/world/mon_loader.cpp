#include "world/mon_loader.h"

#include <algorithm>
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

        float read_f32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<float>(read_u32(data, offset));
        }

        bool read_sized_string(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string& value)
        {
            if (offset + 4 > data.size())
                return false;
            const auto length = read_u32(data, offset);
            offset += 4;
            if (length > 512 || length > data.size() - offset)
                return false;
            value.assign(reinterpret_cast<const char*>(data.data() + offset), length);
            offset += length;
            return true;
        }

        bool is_placeholder(const std::string& value)
        {
            return value.empty() || value == "LOAD" || value == "load";
        }
    }

    MonsterTable load_monster_table(const std::filesystem::path& path)
    {
        MonsterTable table{};
        std::ifstream file(path, std::ios::binary);
        if (!file)
            return table;

        std::vector<std::uint8_t> data{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>() };

        if (data.size() < 7 || data[0] != 'M' || data[1] != 'O' || (data[2] != '2' && data[2] != '4'))
            return table;

        const auto magicVersion = data[2];
        table.declaredCount = read_u32(data, 3);
        std::size_t offset = 7;
        constexpr std::uint32_t kAnimationSlots = 9;
        const std::uint32_t soundSlots = magicVersion == '4' ? 9u : 8u;

        for (std::uint32_t index = 0; index < table.declaredCount && offset < data.size(); ++index)
        {
            MonsterDefinition monster{};
            if (!read_sized_string(data, offset, monster.name))
                break;

            if (offset >= data.size())
                return table;
            ++offset; // Unknown byte.

            for (std::uint32_t i = 0; i < kAnimationSlots; ++i)
            {
                std::string animation;
                if (!read_sized_string(data, offset, animation))
                    return table;
                monster.animationSlots[i] = animation;
                if (!is_placeholder(animation))
                    monster.animationFileNames.push_back(std::move(animation));
            }

            for (std::uint32_t i = 0; i < soundSlots; ++i)
            {
                std::string sound;
                if (!read_sized_string(data, offset, sound))
                    return table;
                if (!is_placeholder(sound))
                    monster.soundFileNames.push_back(std::move(sound));
            }

            if (offset + 4 > data.size())
                break;
            const auto partCount = std::min<std::uint32_t>(read_u32(data, offset), 64);
            offset += 4;
            monster.parts.reserve(partCount);
            for (std::uint32_t i = 0; i < partCount; ++i)
            {
                MonsterPart part{};
                if (!read_sized_string(data, offset, part.meshFileName)
                    || !read_sized_string(data, offset, part.textureFileName))
                    return table;
                if (!part.meshFileName.empty() && !part.textureFileName.empty())
                    monster.parts.push_back(std::move(part));
            }

            if (offset + 4 <= data.size())
            {
                monster.height = read_f32(data, offset);
                monster.scale = monster.height;
                offset += 4;
            }
            if (offset + 4 <= data.size())
            {
                const auto effectCount = read_u32(data, offset);
                offset += 4;
                if (effectCount > 4096 || offset + static_cast<std::size_t>(effectCount) * 8u > data.size())
                    return table;
                offset += static_cast<std::size_t>(effectCount) * 8u;
            }

            table.monsters.push_back(std::move(monster));
        }

        table.parsed = !table.monsters.empty();
        return table;
    }
}
