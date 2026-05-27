#define _CRT_SECURE_NO_WARNINGS
#include "renderer/dds_loader.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace phoenix::renderer
{
    namespace
    {
        std::uint32_t read_u32(const std::uint8_t* ptr)
        {
            std::uint32_t value{};
            std::memcpy(&value, ptr, 4);
            return value;
        }

        std::uint16_t read_u16(const std::uint8_t* ptr)
        {
            std::uint16_t value{};
            std::memcpy(&value, ptr, 2);
            return value;
        }

        std::vector<std::uint8_t> read_file_binary(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
                return {};
            const auto size = static_cast<std::size_t>(file.tellg());
            file.seekg(0);
            std::vector<std::uint8_t> data(size);
            if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
                data.clear();
            return data;
        }

        DdsTexture load_bmp(const std::filesystem::path& path)
        {
            DdsTexture result{};
            auto data = read_file_binary(path);
            if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
                return result;

            const auto pixelOffset = read_u32(data.data() + 10);
            const auto signedWidth = static_cast<std::int32_t>(read_u32(data.data() + 18));
            const auto signedHeight = static_cast<std::int32_t>(read_u32(data.data() + 22));
            const auto planes = read_u16(data.data() + 26);
            const auto bitsPerPixel = read_u16(data.data() + 28);
            const auto compression = read_u32(data.data() + 30);
            if (signedWidth <= 0 || signedHeight == 0 || planes != 1 || compression != 0
                || (bitsPerPixel != 24 && bitsPerPixel != 32))
                return result;

            result.width = static_cast<std::uint32_t>(signedWidth);
            result.height = static_cast<std::uint32_t>(std::abs(signedHeight));
            const auto bytesPerPixel = bitsPerPixel / 8;
            const auto rowStride = ((result.width * bytesPerPixel + 3) / 4) * 4;
            if (pixelOffset + static_cast<std::size_t>(rowStride) * result.height > data.size())
                return result;

            result.rgba.assign(static_cast<std::size_t>(result.width) * result.height * 4, 255);
            const auto topDown = signedHeight < 0;
            for (std::uint32_t y = 0; y < result.height; ++y)
            {
                const auto srcY = topDown ? y : result.height - 1 - y;
                const auto* src = data.data() + pixelOffset + static_cast<std::size_t>(srcY) * rowStride;
                auto* dst = result.rgba.data() + static_cast<std::size_t>(y) * result.width * 4;
                for (std::uint32_t x = 0; x < result.width; ++x)
                {
                    dst[x * 4 + 0] = src[x * bytesPerPixel + 2];
                    dst[x * 4 + 1] = src[x * bytesPerPixel + 1];
                    dst[x * 4 + 2] = src[x * bytesPerPixel + 0];
                    dst[x * 4 + 3] = bytesPerPixel == 4 ? src[x * bytesPerPixel + 3] : 255;
                }
            }

            result.vkFormat = 37;
            result.valid = true;
            return result;
        }

        DdsTexture load_tga(const std::filesystem::path& path)
        {
            DdsTexture result{};
            auto data = read_file_binary(path);
            if (data.size() < 18)
                return result;

            const auto idLength = data[0];
            const auto colorMapType = data[1];
            const auto imageType = data[2];
            const auto width = read_u16(data.data() + 12);
            const auto height = read_u16(data.data() + 14);
            const auto bitsPerPixel = data[16];
            const auto descriptor = data[17];
            if (colorMapType != 0 || (imageType != 2 && imageType != 3) || width == 0 || height == 0
                || (bitsPerPixel != 8 && bitsPerPixel != 24 && bitsPerPixel != 32))
                return result;

            const auto bytesPerPixel = bitsPerPixel / 8;
            const auto pixelOffset = static_cast<std::size_t>(18) + idLength;
            const auto pixelCount = static_cast<std::size_t>(width) * height;
            if (pixelOffset + pixelCount * bytesPerPixel > data.size())
                return result;

            result.width = width;
            result.height = height;
            result.rgba.assign(pixelCount * 4, 255);
            const auto topOrigin = (descriptor & 0x20) != 0;
            for (std::uint32_t y = 0; y < height; ++y)
            {
                const auto srcY = topOrigin ? y : height - 1 - y;
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    const auto src = pixelOffset + (static_cast<std::size_t>(srcY) * width + x) * bytesPerPixel;
                    const auto dst = (static_cast<std::size_t>(y) * width + x) * 4;
                    if (bitsPerPixel == 8)
                    {
                        result.rgba[dst + 0] = data[src];
                        result.rgba[dst + 1] = data[src];
                        result.rgba[dst + 2] = data[src];
                        result.rgba[dst + 3] = 255;
                    }
                    else
                    {
                        result.rgba[dst + 0] = data[src + 2];
                        result.rgba[dst + 1] = data[src + 1];
                        result.rgba[dst + 2] = data[src + 0];
                        result.rgba[dst + 3] = bitsPerPixel == 32 ? data[src + 3] : 255;
                    }
                }
            }

            result.vkFormat = 37;
            result.valid = true;
            return result;
        }

        void rgb565_to_rgba(std::uint16_t c, std::uint8_t* out)
        {
            out[0] = static_cast<std::uint8_t>(((c >> 11) & 0x1F) * 255 / 31);
            out[1] = static_cast<std::uint8_t>(((c >> 5) & 0x3F) * 255 / 63);
            out[2] = static_cast<std::uint8_t>((c & 0x1F) * 255 / 31);
            out[3] = 255;
        }

        void decode_bc1_block(const std::uint8_t* block, std::uint8_t* outPixels)
        {
            const auto c0 = read_u16(block);
            const auto c1 = read_u16(block + 2);
            const auto bits = read_u32(block + 4);

            std::uint8_t palette[4][4]{};
            rgb565_to_rgba(c0, palette[0]);
            rgb565_to_rgba(c1, palette[1]);

            if (c0 > c1)
            {
                for (int i = 0; i < 3; ++i)
                {
                    palette[2][i] = static_cast<std::uint8_t>((2 * palette[0][i] + palette[1][i]) / 3);
                    palette[3][i] = static_cast<std::uint8_t>((palette[0][i] + 2 * palette[1][i]) / 3);
                }
                palette[2][3] = 255;
                palette[3][3] = 255;
            }
            else
            {
                for (int i = 0; i < 3; ++i)
                    palette[2][i] = static_cast<std::uint8_t>((palette[0][i] + palette[1][i]) / 2);
                palette[2][3] = 255;
                palette[3][3] = 0;
            }

            for (int pixel = 0; pixel < 16; ++pixel)
            {
                const auto index = (bits >> (pixel * 2)) & 0x3;
                std::memcpy(outPixels + pixel * 4, palette[index], 4);
            }
        }

        void decode_bc3_alpha(const std::uint8_t* block, std::uint8_t* alphaOut)
        {
            const auto a0 = block[0];
            const auto a1 = block[1];

            std::uint8_t palette[8]{};
            palette[0] = a0;
            palette[1] = a1;
            if (a0 > a1)
            {
                for (int i = 1; i <= 6; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(((7 - i) * a0 + i * a1) / 7);
            }
            else
            {
                for (int i = 1; i <= 4; ++i)
                    palette[i + 1] = static_cast<std::uint8_t>(((5 - i) * a0 + i * a1) / 5);
                palette[6] = 0;
                palette[7] = 255;
            }

            std::uint64_t bits = 0;
            for (int i = 0; i < 6; ++i)
                bits |= static_cast<std::uint64_t>(block[2 + i]) << (i * 8);

            for (int pixel = 0; pixel < 16; ++pixel)
            {
                const auto index = (bits >> (pixel * 3)) & 0x7;
                alphaOut[pixel] = palette[index];
            }
        }

        void decode_bc2_alpha(const std::uint8_t* block, std::uint8_t* alphaOut)
        {
            for (int pixel = 0; pixel < 16; ++pixel)
            {
                const auto byte = block[pixel / 2];
                const auto nibble = (pixel & 1) == 0 ? (byte & 0x0F) : (byte >> 4);
                alphaOut[pixel] = static_cast<std::uint8_t>(nibble * 17);
            }
        }

        void decompress_bc1(const std::uint8_t* src, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& rgba)
        {
            const auto blocksW = std::max(1u, (width + 3) / 4);
            const auto blocksH = std::max(1u, (height + 3) / 4);
            rgba.resize(static_cast<std::size_t>(width) * height * 4);

            for (std::uint32_t by = 0; by < blocksH; ++by)
            {
                for (std::uint32_t bx = 0; bx < blocksW; ++bx)
                {
                    std::uint8_t pixels[16 * 4]{};
                    decode_bc1_block(src + (static_cast<std::size_t>(by) * blocksW + bx) * 8, pixels);

                    for (int py = 0; py < 4; ++py)
                    {
                        const auto row = by * 4 + py;
                        if (row >= height)
                            break;
                        for (int px = 0; px < 4; ++px)
                        {
                            const auto col = bx * 4 + px;
                            if (col >= width)
                                break;
                            const auto dst = (static_cast<std::size_t>(row) * width + col) * 4;
                            std::memcpy(rgba.data() + dst, pixels + (py * 4 + px) * 4, 4);
                        }
                    }
                }
            }
        }

        void decompress_bc2(const std::uint8_t* src, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& rgba)
        {
            const auto blocksW = std::max(1u, (width + 3) / 4);
            const auto blocksH = std::max(1u, (height + 3) / 4);
            rgba.resize(static_cast<std::size_t>(width) * height * 4);

            for (std::uint32_t by = 0; by < blocksH; ++by)
            {
                for (std::uint32_t bx = 0; bx < blocksW; ++bx)
                {
                    const auto blockOffset = (static_cast<std::size_t>(by) * blocksW + bx) * 16;
                    std::uint8_t alphas[16]{};
                    decode_bc2_alpha(src + blockOffset, alphas);

                    std::uint8_t pixels[16 * 4]{};
                    decode_bc1_block(src + blockOffset + 8, pixels);

                    for (int i = 0; i < 16; ++i)
                        pixels[i * 4 + 3] = alphas[i];

                    for (int py = 0; py < 4; ++py)
                    {
                        const auto row = by * 4 + py;
                        if (row >= height)
                            break;
                        for (int px = 0; px < 4; ++px)
                        {
                            const auto col = bx * 4 + px;
                            if (col >= width)
                                break;
                            const auto dst = (static_cast<std::size_t>(row) * width + col) * 4;
                            std::memcpy(rgba.data() + dst, pixels + (py * 4 + px) * 4, 4);
                        }
                    }
                }
            }
        }

        void decompress_bc3(const std::uint8_t* src, std::uint32_t width, std::uint32_t height, std::vector<std::uint8_t>& rgba)
        {
            const auto blocksW = std::max(1u, (width + 3) / 4);
            const auto blocksH = std::max(1u, (height + 3) / 4);
            rgba.resize(static_cast<std::size_t>(width) * height * 4);

            for (std::uint32_t by = 0; by < blocksH; ++by)
            {
                for (std::uint32_t bx = 0; bx < blocksW; ++bx)
                {
                    const auto blockOffset = (static_cast<std::size_t>(by) * blocksW + bx) * 16;
                    std::uint8_t alphas[16]{};
                    decode_bc3_alpha(src + blockOffset, alphas);

                    std::uint8_t pixels[16 * 4]{};
                    decode_bc1_block(src + blockOffset + 8, pixels);

                    for (int i = 0; i < 16; ++i)
                        pixels[i * 4 + 3] = alphas[i];

                    for (int py = 0; py < 4; ++py)
                    {
                        const auto row = by * 4 + py;
                        if (row >= height)
                            break;
                        for (int px = 0; px < 4; ++px)
                        {
                            const auto col = bx * 4 + px;
                            if (col >= width)
                                break;
                            const auto dst = (static_cast<std::size_t>(row) * width + col) * 4;
                            std::memcpy(rgba.data() + dst, pixels + (py * 4 + px) * 4, 4);
                        }
                    }
                }
            }
        }
    }

    DdsTexture load_dds(const std::filesystem::path& path)
    {
        const auto extension = path.extension().string();
        if (extension == ".bmp" || extension == ".BMP")
            return load_bmp(path);
        if (extension == ".tga" || extension == ".TGA")
            return load_tga(path);

        DdsTexture result{};

        auto data = read_file_binary(path);
        if (data.size() < 128)
            return result;

        if (read_u32(data.data()) != 0x20534444u)
            return result;

        result.height = read_u32(data.data() + 12);
        result.width = read_u32(data.data() + 16);
        if (result.width == 0 || result.height == 0)
            return result;

        const auto fourCC = read_u32(data.data() + 84);
        constexpr std::size_t kPayloadOffset = 128;

        if (fourCC == 0x31545844u) // DXT1
        {
            const auto blocksW = std::max(1u, (result.width + 3) / 4);
            const auto blocksH = std::max(1u, (result.height + 3) / 4);
            const auto compressedSize = static_cast<std::size_t>(blocksW) * blocksH * 8;
            if (kPayloadOffset + compressedSize > data.size())
                return result;
            decompress_bc1(data.data() + kPayloadOffset, result.width, result.height, result.rgba);
        }
        else if (fourCC == 0x33545844u || fourCC == 0x35545844u) // DXT3 or DXT5
        {
            const auto blocksW = std::max(1u, (result.width + 3) / 4);
            const auto blocksH = std::max(1u, (result.height + 3) / 4);
            const auto compressedSize = static_cast<std::size_t>(blocksW) * blocksH * 16;
            if (kPayloadOffset + compressedSize > data.size())
                return result;
            if (fourCC == 0x33545844u)
                decompress_bc2(data.data() + kPayloadOffset, result.width, result.height, result.rgba);
            else
                decompress_bc3(data.data() + kPayloadOffset, result.width, result.height, result.rgba);
        }
        else
        {
            const auto bitCount = read_u32(data.data() + 88);
            if (bitCount == 32)
            {
                const auto size = static_cast<std::size_t>(result.width) * result.height * 4;
                if (kPayloadOffset + size > data.size())
                    return result;

                const auto rMask = read_u32(data.data() + 92);
                result.rgba.resize(size);
                if (rMask == 0x00FF0000u) // BGRA → RGBA
                {
                    for (std::size_t i = 0; i < static_cast<std::size_t>(result.width) * result.height; ++i)
                    {
                        const auto s = kPayloadOffset + i * 4;
                        result.rgba[i * 4 + 0] = data[s + 2];
                        result.rgba[i * 4 + 1] = data[s + 1];
                        result.rgba[i * 4 + 2] = data[s + 0];
                        result.rgba[i * 4 + 3] = data[s + 3];
                    }
                }
                else
                {
                    std::memcpy(result.rgba.data(), data.data() + kPayloadOffset, size);
                }
            }
            else
            {
                return result;
            }
        }

        result.vkFormat = 37; // VK_FORMAT_R8G8B8A8_UNORM
        result.valid = !result.rgba.empty();
        return result;
    }
}
