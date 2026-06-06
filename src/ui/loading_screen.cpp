#include "ui/loading_screen.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace phoenix::ui
{
    namespace
    {
        std::array<std::uint8_t, 7> glyph_rows(char ch)
        {
            switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch))))
            {
            case 'A': return { 0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 };
            case 'B': return { 0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e };
            case 'C': return { 0x0f, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0f };
            case 'D': return { 0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e };
            case 'E': return { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f };
            case 'F': return { 0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10 };
            case 'G': return { 0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0e };
            case 'H': return { 0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11 };
            case 'I': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f };
            case 'J': return { 0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e };
            case 'K': return { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 };
            case 'L': return { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f };
            case 'M': return { 0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11 };
            case 'N': return { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 };
            case 'O': return { 0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'P': return { 0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10 };
            case 'Q': return { 0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d };
            case 'R': return { 0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11 };
            case 'S': return { 0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e };
            case 'T': return { 0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
            case 'U': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e };
            case 'V': return { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04 };
            case 'W': return { 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a };
            case 'X': return { 0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11 };
            case 'Y': return { 0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04 };
            case 'Z': return { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f };
            case ' ': return { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            default: return {0, 0, 0, 0, 0, 0, 0};
            }
        }

        std::uint32_t text_pixel_width(std::string_view text, std::uint32_t scale)
        {
            if (text.empty())
                return 0;
            return static_cast<std::uint32_t>(text.size()) * 6u * scale - scale;
        }

        void draw_text(
            std::vector<std::uint8_t>& pixels,
            std::uint32_t imgWidth,
            std::uint32_t imgHeight,
            std::string_view text,
            std::uint32_t startX,
            std::uint32_t startY,
            std::uint32_t scale,
            std::uint8_t b,
            std::uint8_t g,
            std::uint8_t r)
        {
            if (scale == 0)
                return;
            auto cursorX = startX;
            for (const char ch : text)
            {
                const auto rows = glyph_rows(ch);
                for (std::uint32_t row = 0; row < rows.size(); ++row)
                    for (std::uint32_t col = 0; col < 5u; ++col)
                    {
                        if ((rows[row] & (1u << (4u - col))) == 0)
                            continue;
                        for (std::uint32_t py = startY + row * scale; py < startY + (row + 1u) * scale; ++py)
                            for (std::uint32_t px = cursorX + col * scale; px < cursorX + (col + 1u) * scale; ++px)
                            {
                                if (px >= imgWidth || py >= imgHeight)
                                    continue;
                                const auto offset = (static_cast<std::size_t>(py) * imgWidth + px) * 4u;
                                pixels[offset + 0] = b;
                                pixels[offset + 1] = g;
                                pixels[offset + 2] = r;
                                pixels[offset + 3] = 255;
                            }
                    }
                cursorX += 6u * scale;
            }
        }
    }

    std::vector<std::uint8_t> make_loading_image(
        std::uint32_t width,
        std::uint32_t height)
    {
        width = std::max(1u, width);
        height = std::max(1u, height);

        // Solid dark background.
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4u);
        for (std::size_t i = 0; i < pixels.size(); i += 4)
        {
            pixels[i + 0] = 22;
            pixels[i + 1] = 16;
            pixels[i + 2] = 12;
            pixels[i + 3] = 255;
        }

        // "Loading the engine" centered.
        constexpr std::string_view label = "Loading the engine";
        const auto textScale = std::clamp(height / 360u, 2u, 4u);
        const auto textWidth = text_pixel_width(label, textScale);
        const auto textHeight = 7u * textScale;
        const auto textX = width > textWidth ? (width - textWidth) / 2u : 0u;
        const auto textY = height > textHeight ? (height - textHeight) / 2u : 0u;
        draw_text(pixels, width, height, label, textX, textY, textScale, 210, 215, 220);

        return pixels;
    }
}
