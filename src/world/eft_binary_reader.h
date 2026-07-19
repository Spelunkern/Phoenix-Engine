#pragma once

// Shared little-endian reader for the .EFT/.EF2/.EF3 effect format and its
// companion .3DE mesh format. Not a public interface: included only by
// eft_loader.cpp and eft_mesh_loader.cpp.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phoenix::world::detail
{
    // Sequential reader over an in-memory buffer. Every read clamps to the
    // buffer bounds and latches `ok = false` on the first failure instead of
    // throwing, matching the rest of the world loaders.
    struct EftReader
    {
        const std::vector<std::uint8_t>& data;
        std::size_t offset{};
        bool ok{ true };

        std::uint32_t u32()
        {
            if (!ok || offset + 4 > data.size())
            {
                ok = false;
                return 0;
            }
            const auto value = static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
            offset += 4;
            return value;
        }

        std::uint8_t u8()
        {
            if (!ok || offset + 1 > data.size())
            {
                ok = false;
                return 0;
            }
            return data[offset++];
        }

        std::uint16_t u16()
        {
            if (!ok || offset + 2 > data.size())
            {
                ok = false;
                return 0;
            }
            const auto value = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(data[offset])
                | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
            offset += 2;
            return value;
        }

        std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

        float f32() { return std::bit_cast<float>(u32()); }

        void vec3(float (&out)[3])
        {
            out[0] = f32();
            out[1] = f32();
            out[2] = f32();
        }

        void vec2(float (&out)[2])
        {
            out[0] = f32();
            out[1] = f32();
        }

        // A length-prefixed string: u32 byte count, then that many bytes. A
        // single trailing NUL (if present) is stripped, matching the
        // effect-renderer reference parser's modelString().
        std::string string(std::uint32_t maxLength)
        {
            const auto length = u32();
            if (!ok || length > maxLength || offset + length > data.size())
            {
                ok = false;
                return {};
            }

            std::size_t end = length;
            if (end > 0 && data[offset + end - 1] == 0)
                --end;

            std::string value(reinterpret_cast<const char*>(data.data() + offset), end);
            offset += length;
            return value;
        }

        // A length-prefixed UTF-16LE string (used by .SEFF, unlike .EFT's
        // single-byte-per-char strings): u32 CHARACTER count (not byte
        // count), then that many 16-bit code units. Effect texture
        // filenames are plain ASCII in practice, so non-ASCII code units
        // are replaced rather than properly UTF-8 re-encoded.
        std::string stringUtf16(std::uint32_t maxChars)
        {
            const auto charCount = u32();
            if (!ok || charCount > maxChars || offset + static_cast<std::size_t>(charCount) * 2 > data.size())
            {
                ok = false;
                return {};
            }
            std::string value;
            value.reserve(charCount);
            for (std::uint32_t i = 0; i < charCount; ++i)
            {
                const auto code = u16();
                value.push_back(code < 0x80 ? static_cast<char>(code) : '?');
            }
            if (!value.empty() && value.back() == '\0')
                value.pop_back();
            return value;
        }

        // Reads and validates a list count, capping it to `maximum` so a
        // corrupt/foreign file can't trigger a huge allocation.
        std::uint32_t count(std::uint32_t maximum)
        {
            const auto value = u32();
            if (!ok || value > maximum)
            {
                ok = false;
                return 0;
            }
            return value;
        }

        bool remaining() const { return offset < data.size(); }
    };
}
