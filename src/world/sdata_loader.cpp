#include "world/sdata_loader.h"

#include "world/seed_constants.inc"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace phoenix::world
{
    namespace
    {
        constexpr std::string_view kSeedSignature = "0001CBCEBC5B2784D3FC9A2A9DB84D1C3FEB6E99";

        std::uint32_t read_u32_le(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 4 > data.size())
                return 0;
            return static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

        std::uint32_t read_u32_be(const std::uint8_t* data)
        {
            return (static_cast<std::uint32_t>(data[0]) << 24)
                | (static_cast<std::uint32_t>(data[1]) << 16)
                | (static_cast<std::uint32_t>(data[2]) << 8)
                | static_cast<std::uint32_t>(data[3]);
        }

        std::uint16_t read_u16_le(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 2 > data.size())
                return 0;
            return static_cast<std::uint16_t>(data[offset])
                | (static_cast<std::uint16_t>(data[offset + 1]) << 8);
        }

        std::int16_t read_i16_le(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<std::int16_t>(read_u16_le(data, offset));
        }

        std::int32_t read_i32_le(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<std::int32_t>(read_u32_le(data, offset));
        }

        std::uint32_t rotl(std::uint32_t x, int n)
        {
            return (x << n) | (x >> (32 - n));
        }

        void endian_swap(std::uint32_t& value)
        {
            const auto value1 = rotl(value, 8) & 0x00ff00ffu;
            const auto value2 = rotl(value, 24) & 0xff00ff00u;
            value = value1 | value2;
        }

        std::uint32_t get_seed(std::uint32_t value)
        {
            return seed_constants::SS[0][static_cast<std::uint8_t>(value)]
                ^ seed_constants::SS[1][static_cast<std::uint8_t>(value >> 8)]
                ^ seed_constants::SS[2][static_cast<std::uint8_t>(value >> 16)]
                ^ seed_constants::SS[3][static_cast<std::uint8_t>(value >> 24)];
        }

        void seed_round(
            std::uint32_t& l0,
            std::uint32_t& l1,
            std::uint32_t r0,
            std::uint32_t r1,
            std::uint32_t offset)
        {
            std::uint32_t k0 = read_u32_be(seed_constants::Key + offset * 4u);
            std::uint32_t k1 = read_u32_be(seed_constants::Key + (offset + 1u) * 4u);
            endian_swap(k0);
            endian_swap(k1);

            auto t0 = r0 ^ k0;
            auto t1 = r1 ^ k1;
            t1 ^= t0;
            t1 = get_seed(t1);
            t0 += t1;
            t0 = get_seed(t0);
            t1 += t0;
            t1 = get_seed(t1);
            t0 += t1;
            l0 ^= t0;
            l1 ^= t1;
        }

        void write_u32_be(std::uint32_t value, std::uint8_t* out)
        {
            out[0] = static_cast<std::uint8_t>(value >> 24);
            out[1] = static_cast<std::uint8_t>(value >> 16);
            out[2] = static_cast<std::uint8_t>(value >> 8);
            out[3] = static_cast<std::uint8_t>(value);
        }

        void decrypt_seed_chunk(const std::uint8_t* input, std::uint8_t* output)
        {
            auto l0 = read_u32_be(input + 0);
            auto l1 = read_u32_be(input + 4);
            auto r0 = read_u32_be(input + 8);
            auto r1 = read_u32_be(input + 12);

            for (std::uint32_t round = 0; round < 16; ++round)
            {
                const auto offset = 30u - round * 2u;
                if ((round & 1u) == 0)
                    seed_round(l0, l1, r0, r1, offset);
                else
                    seed_round(r0, r1, l0, l1, offset);
            }

            write_u32_be(r0, output + 0);
            write_u32_be(r1, output + 4);
            write_u32_be(l0, output + 8);
            write_u32_be(l1, output + 12);
        }

        std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
                return {};
            return {
                std::istreambuf_iterator<char>(file),
                std::istreambuf_iterator<char>() };
        }

        std::vector<std::uint8_t> decrypt_sdata(const std::filesystem::path& path)
        {
            auto input = read_file(path);
            if (input.size() < 64)
                return {};
            if (std::string_view(reinterpret_cast<const char*>(input.data()), kSeedSignature.size()) != kSeedSignature)
                return input;

            std::size_t headerOffset = 40;
            auto checksum = read_u32_le(input, headerOffset);
            headerOffset += 4;
            if (checksum == 0)
            {
                checksum = read_u32_le(input, headerOffset);
                headerOffset += 4;
            }
            (void)checksum;
            const auto realSize = read_u32_le(input, headerOffset);
            if (realSize == 0 || input.size() < 64 || (input.size() - 64) % 16 != 0)
                return {};

            std::vector<std::uint8_t> decrypted(input.size() - 64);
            for (std::size_t offset = 64; offset + 16 <= input.size(); offset += 16)
                decrypt_seed_chunk(input.data() + offset, decrypted.data() + offset - 64);

            if (realSize > decrypted.size())
                return {};
            decrypted.resize(realSize);
            return decrypted;
        }

        bool read_string(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string& out)
        {
            if (offset + 4 > data.size())
                return false;
            const auto length = read_u32_le(data, offset);
            offset += 4;
            if (length > 4096 || length > data.size() - offset)
                return false;
            out.assign(reinterpret_cast<const char*>(data.data() + offset), length);
            while (!out.empty() && out.back() == '\0')
                out.pop_back();
            offset += length;
            return true;
        }

        bool skip_quests(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            if (offset + 4 > data.size())
                return false;
            const auto count = read_u32_le(data, offset);
            offset += 4;
            if (count > 10000 || offset + static_cast<std::size_t>(count) * 2u > data.size())
                return false;
            offset += static_cast<std::size_t>(count) * 2u;
            return true;
        }

        bool read_npc_base_second(const std::vector<std::uint8_t>& data, std::size_t& offset, NpcSDataRecord& record)
        {
            if (offset + 16 > data.size())
                return false;
            record.modelId = read_i32_le(data, offset);
            offset += 16; // model, move distance, move speed, faction.
            std::string welcome;
            return read_string(data, offset, record.name) && read_string(data, offset, welcome);
        }

        bool read_npc_base_tail(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            return skip_quests(data, offset) && skip_quests(data, offset);
        }

        bool store_record(
            NpcQuestSData& out,
            std::uint8_t type,
            std::int16_t typeId,
            NpcSDataRecord record)
        {
            const auto key = (static_cast<std::uint32_t>(type) << 16) | static_cast<std::uint16_t>(typeId);
            out.records[key] = std::move(record);
            return true;
        }

        bool read_standard_npc_list(const std::vector<std::uint8_t>& data, std::size_t& offset, NpcQuestSData& out)
        {
            if (offset + 4 > data.size())
                return false;
            const auto count = read_u32_le(data, offset);
            offset += 4;
            if (count > 10000)
                return false;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (offset + 3 > data.size())
                    return false;
                const auto type = data[offset++];
                const auto typeId = read_i16_le(data, offset);
                offset += 2;
                NpcSDataRecord record{};
                if (!read_npc_base_second(data, offset, record) || !read_npc_base_tail(data, offset))
                    return false;
                store_record(out, type, typeId, std::move(record));
            }
            return true;
        }

        bool read_merchant_list(const std::vector<std::uint8_t>& data, std::size_t& offset, NpcQuestSData& out)
        {
            if (offset + 4 > data.size())
                return false;
            const auto count = read_u32_le(data, offset);
            offset += 4;
            if (count > 10000)
                return false;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (offset + 4 > data.size())
                    return false;
                const auto type = data[offset++];
                const auto typeId = read_i16_le(data, offset);
                offset += 2;
                ++offset; // merchant type.
                NpcSDataRecord record{};
                if (!read_npc_base_second(data, offset, record))
                    return false;
                if (offset + 4 > data.size())
                    return false;
                const auto itemCount = read_u32_le(data, offset);
                offset += 4;
                if (itemCount > 10000 || offset + static_cast<std::size_t>(itemCount) * 2u > data.size())
                    return false;
                offset += static_cast<std::size_t>(itemCount) * 2u;
                if (!read_npc_base_tail(data, offset))
                    return false;
                store_record(out, type, typeId, std::move(record));
            }
            return true;
        }

        bool read_gatekeeper_list(const std::vector<std::uint8_t>& data, std::size_t& offset, NpcQuestSData& out)
        {
            if (offset + 4 > data.size())
                return false;
            const auto count = read_u32_le(data, offset);
            offset += 4;
            if (count > 10000)
                return false;
            for (std::uint32_t i = 0; i < count; ++i)
            {
                if (offset + 3 > data.size())
                    return false;
                const auto type = data[offset++];
                const auto typeId = read_i16_le(data, offset);
                offset += 2;
                NpcSDataRecord record{};
                if (!read_npc_base_second(data, offset, record))
                    return false;
                for (int g = 0; g < 3; ++g)
                {
                    if (offset + 18 > data.size())
                        return false;
                    offset += 14; // map id + vector3.
                    std::string target;
                    if (!read_string(data, offset, target) || offset + 4 > data.size())
                        return false;
                    offset += 4;
                }
                if (!read_npc_base_tail(data, offset))
                    return false;
                store_record(out, type, typeId, std::move(record));
            }
            return true;
        }
    }

    MonsterSData load_monster_sdata(const std::filesystem::path& path)
    {
        MonsterSData out{};
        auto data = decrypt_sdata(path);
        if (data.size() < 4)
            return out;

        std::size_t offset = 0;
        const auto count = read_u32_le(data, offset);
        offset += 4;
        if (count > 100000)
            return out;

        for (std::uint32_t id = 0; id < count; ++id)
        {
            MonsterSDataRecord record{};
            if (!read_string(data, offset, record.name) || offset + 31 > data.size())
                return out;
            record.modelId = read_i16_le(data, offset);
            offset += 2;
            offset += 2; // level
            offset += 1; // ai
            offset += 4; // hp
            offset += 1; // day
            record.size = data[offset++];
            offset += 1; // element
            offset += 4; // normal time
            offset += 1; // normal step
            offset += 4; // chase time
            offset += 1; // chase step
            offset += 7; // attack fields
            offset += 2; // quest item id
            out.records[id] = std::move(record);
        }

        out.parsed = !out.records.empty();
        return out;
    }

    NpcQuestSData load_npcquest_sdata(const std::filesystem::path& path)
    {
        NpcQuestSData out{};
        auto data = decrypt_sdata(path);
        if (data.size() < 4)
            return out;

        std::size_t offset = 0;
        if (!read_merchant_list(data, offset, out)
            || !read_gatekeeper_list(data, offset, out))
        {
            return out;
        }

        for (int i = 0; i < 11; ++i)
        {
            if (!read_standard_npc_list(data, offset, out))
                return out;
        }

        out.parsed = !out.records.empty();
        return out;
    }
}
