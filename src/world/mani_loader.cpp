#include "world/mani_loader.h"

#include "assets/data_index.h"

#include <bit>
#include <cstdint>
#include <fstream>
#include <vector>

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
    }

    ManiAnimation load_mani(const std::filesystem::path& path)
    {
        ManiAnimation animation{};
        auto data = assets::read_file_binary(path);
        if (data.size() < 96)
            return animation;

        const auto version = read_u32(data, 0);
        const auto enableRotation = read_u32(data, 60);
        animation.enableRotation = enableRotation != 0;
        animation.rotationAxis[0] = read_f32(data, 64);
        animation.rotationAxis[1] = read_f32(data, 68);
        animation.rotationAxis[2] = read_f32(data, 72);
        animation.animationSpeed = read_f32(data, 76);
        animation.parsed = version == 33 || animation.enableRotation;
        return animation;
    }
}
