#pragma once

#include <cstdint>
#include <vector>

namespace phoenix::ui
{
    std::vector<std::uint8_t> make_loading_image(
        std::uint32_t width,
        std::uint32_t height);
}
