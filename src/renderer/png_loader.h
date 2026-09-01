#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace phoenix::renderer
{
    // Decoded PNG image, always normalised to 8-bit RGBA (matches the layout
    // GPU upload paths expect for UI icons/interface art).
    struct PngImage
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> rgba; // 4 bytes/pixel, row-major, top-to-bottom
        bool valid{};
    };

    PngImage load_png(const std::filesystem::path& path);
}
