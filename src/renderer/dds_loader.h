#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace phoenix::renderer
{
    struct DdsTexture
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::uint32_t vkFormat{};
        std::vector<std::uint8_t> rgba;
        bool valid{};
    };

    DdsTexture load_dds(const std::filesystem::path& path);
}
