#define _CRT_SECURE_NO_WARNINGS
#include "renderer/dds_loader.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>

namespace phoenix::renderer
{
    namespace
    {
        constexpr std::uint32_t kVkFormatR8G8B8A8Unorm = 37;
        constexpr std::uint32_t kVkFormatBc1RgbaUnormBlock = 133;
        constexpr std::uint32_t kVkFormatBc2UnormBlock = 135;
        constexpr std::uint32_t kVkFormatBc3UnormBlock = 137;

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

            result.vkFormat = kVkFormatR8G8B8A8Unorm;
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

            result.vkFormat = kVkFormatR8G8B8A8Unorm;
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

        std::vector<std::vector<std::uint8_t>> read_bc_mips(
            const std::vector<std::uint8_t>& data,
            std::size_t payloadOffset,
            std::uint32_t width,
            std::uint32_t height,
            std::uint32_t mipCount,
            std::uint32_t blockBytes)
        {
            std::vector<std::vector<std::uint8_t>> mips;
            mipCount = std::max(1u, mipCount);
            mips.reserve(mipCount);
            auto offset = payloadOffset;
            for (std::uint32_t mip = 0; mip < mipCount; ++mip)
            {
                const auto mipWidth = std::max(1u, width >> mip);
                const auto mipHeight = std::max(1u, height >> mip);
                const auto blocksW = std::max(1u, (mipWidth + 3u) / 4u);
                const auto blocksH = std::max(1u, (mipHeight + 3u) / 4u);
                const auto mipSize = static_cast<std::size_t>(blocksW) * blocksH * blockBytes;
                if (offset + mipSize > data.size())
                    break;
                mips.emplace_back(data.begin() + static_cast<std::ptrdiff_t>(offset),
                    data.begin() + static_cast<std::ptrdiff_t>(offset + mipSize));
                offset += mipSize;
            }
            return mips;
        }
    }

    DdsTexture load_dds(const std::filesystem::path& path)
    {
        const auto extension = path.extension().string();
        if (extension != ".dds" && extension != ".DDS")
            return {};

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

        const auto mipCount = std::max(1u, read_u32(data.data() + 28));
        const auto fourCC = read_u32(data.data() + 84);
        constexpr std::size_t kPayloadOffset = 128;

        if (fourCC == 0x31545844u) // DXT1
        {
            const auto blocksW = std::max(1u, (result.width + 3) / 4);
            const auto blocksH = std::max(1u, (result.height + 3) / 4);
            const auto compressedSize = static_cast<std::size_t>(blocksW) * blocksH * 8;
            if (kPayloadOffset + compressedSize > data.size())
                return result;
            result.vkFormat = kVkFormatBc1RgbaUnormBlock;
            result.blockBytes = 8;
            result.compressed = true;
            result.mipData = read_bc_mips(data, kPayloadOffset, result.width, result.height, mipCount, result.blockBytes);
        }
        else if (fourCC == 0x33545844u || fourCC == 0x35545844u) // DXT3 or DXT5
        {
            const auto blocksW = std::max(1u, (result.width + 3) / 4);
            const auto blocksH = std::max(1u, (result.height + 3) / 4);
            const auto compressedSize = static_cast<std::size_t>(blocksW) * blocksH * 16;
            if (kPayloadOffset + compressedSize > data.size())
                return result;
            result.vkFormat = fourCC == 0x33545844u ? kVkFormatBc2UnormBlock : kVkFormatBc3UnormBlock;
            result.blockBytes = 16;
            result.compressed = true;
            result.mipData = read_bc_mips(data, kPayloadOffset, result.width, result.height, mipCount, result.blockBytes);
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

        if (result.vkFormat == 0)
            result.vkFormat = kVkFormatR8G8B8A8Unorm;
        result.valid = result.compressed ? !result.mipData.empty() : !result.rgba.empty();
        return result;
    }

    std::vector<std::uint8_t> decode_texture_rgba(const DdsTexture& texture)
    {
        if (!texture.rgba.empty())
            return texture.rgba;
        if (!texture.valid || !texture.compressed || texture.mipData.empty())
            return {};

        std::vector<std::uint8_t> rgba;
        if (texture.vkFormat == kVkFormatBc1RgbaUnormBlock)
            decompress_bc1(texture.mipData[0].data(), texture.width, texture.height, rgba);
        else if (texture.vkFormat == kVkFormatBc2UnormBlock)
            decompress_bc2(texture.mipData[0].data(), texture.width, texture.height, rgba);
        else if (texture.vkFormat == kVkFormatBc3UnormBlock)
            decompress_bc3(texture.mipData[0].data(), texture.width, texture.height, rgba);
        return rgba;
    }

    // ── BC1 → BC3 lossless conversion ────────────────────────────────────
    void convert_bc1_to_bc3(DdsTexture& texture)
    {
        if (!texture.compressed || texture.vkFormat != kVkFormatBc1RgbaUnormBlock)
            return;
        for (auto& mip : texture.mipData)
        {
            const auto bc1Blocks = mip.size() / 8;
            std::vector<std::uint8_t> bc3(bc1Blocks * 16);
            for (std::size_t i = 0; i < bc1Blocks; ++i)
            {
                auto* dst = bc3.data() + i * 16;
                // Opaque alpha block: alpha0=255, alpha1=255, all indices 0.
                dst[0] = 0xFF;
                dst[1] = 0xFF;
                std::memset(dst + 2, 0, 6);
                // Copy the BC1 colour block unchanged.
                std::memcpy(dst + 8, mip.data() + i * 8, 8);
            }
            mip = std::move(bc3);
        }
        texture.vkFormat = kVkFormatBc3UnormBlock;
        texture.blockBytes = 16;
    }

    // ── Simple BC3 encoder from RGBA ─────────────────────────────────────
    namespace
    {
        std::uint16_t rgb565(std::uint8_t r, std::uint8_t g, std::uint8_t b)
        {
            return static_cast<std::uint16_t>(
                (static_cast<unsigned>(r >> 3) << 11) |
                (static_cast<unsigned>(g >> 2) << 5) |
                static_cast<unsigned>(b >> 3));
        }

        void decode565(std::uint16_t c, std::uint8_t rgb[3])
        {
            rgb[0] = static_cast<std::uint8_t>(((c >> 11) * 255 + 15) / 31);
            rgb[1] = static_cast<std::uint8_t>((((c >> 5) & 63) * 255 + 31) / 63);
            rgb[2] = static_cast<std::uint8_t>(((c & 31) * 255 + 15) / 31);
        }

        void encode_one_bc3_block(const std::uint8_t* rgba, std::uint32_t stride, std::uint8_t* out)
        {
            // Gather 4×4 pixel block.
            std::uint8_t px[16][4];
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x)
                    std::memcpy(px[y * 4 + x], rgba + y * stride + x * 4, 4);

            // ── Alpha block (8 bytes) ──
            std::uint8_t minA = 255, maxA = 0;
            for (auto& p : px)
            {
                if (p[3] < minA) minA = p[3];
                if (p[3] > maxA) maxA = p[3];
            }
            out[0] = maxA;
            out[1] = minA;
            std::uint64_t aBits = 0;
            if (maxA == minA)
            {
                // All same alpha → indices all 0.
                std::memset(out + 2, 0, 6);
            }
            else
            {
                for (int i = 0; i < 16; ++i)
                {
                    const int a = px[i][3];
                    int bestDist = INT_MAX, bestIdx = 0;
                    for (int j = 0; j < 8; ++j)
                    {
                        int palA;
                        if (j == 0) palA = maxA;
                        else if (j == 1) palA = minA;
                        else palA = ((8 - j) * maxA + (j - 1) * minA + 3) / 7;
                        const int d = std::abs(a - palA);
                        if (d < bestDist) { bestDist = d; bestIdx = j; }
                    }
                    aBits |= static_cast<std::uint64_t>(bestIdx) << (i * 3);
                }
                for (int i = 0; i < 6; ++i)
                    out[2 + i] = static_cast<std::uint8_t>((aBits >> (i * 8)) & 0xFF);
            }

            // ── Colour block / BC1 (8 bytes at out+8) ──
            std::uint8_t rMin = 255, gMin = 255, bMin = 255;
            std::uint8_t rMax = 0, gMax = 0, bMax = 0;
            for (auto& p : px)
            {
                if (p[0] < rMin) rMin = p[0]; if (p[0] > rMax) rMax = p[0];
                if (p[1] < gMin) gMin = p[1]; if (p[1] > gMax) gMax = p[1];
                if (p[2] < bMin) bMin = p[2]; if (p[2] > bMax) bMax = p[2];
            }

            auto c0 = rgb565(rMax, gMax, bMax);
            auto c1 = rgb565(rMin, gMin, bMin);
            if (c0 < c1) std::swap(c0, c1);
            if (c0 == c1 && c0 < 0xFFFFu) ++c0;

            out[8]  = static_cast<std::uint8_t>(c0 & 0xFF);
            out[9]  = static_cast<std::uint8_t>(c0 >> 8);
            out[10] = static_cast<std::uint8_t>(c1 & 0xFF);
            out[11] = static_cast<std::uint8_t>(c1 >> 8);

            std::uint8_t pal[4][3];
            decode565(c0, pal[0]);
            decode565(c1, pal[1]);
            for (int j = 0; j < 3; ++j)
            {
                pal[2][j] = static_cast<std::uint8_t>((2 * pal[0][j] + pal[1][j] + 1) / 3);
                pal[3][j] = static_cast<std::uint8_t>((pal[0][j] + 2 * pal[1][j] + 1) / 3);
            }

            std::uint32_t indices = 0;
            for (int i = 0; i < 16; ++i)
            {
                int bestDist = INT_MAX, bestIdx = 0;
                for (int j = 0; j < 4; ++j)
                {
                    const int dr = px[i][0] - pal[j][0];
                    const int dg = px[i][1] - pal[j][1];
                    const int db = px[i][2] - pal[j][2];
                    const int d = dr * dr + dg * dg + db * db;
                    if (d < bestDist) { bestDist = d; bestIdx = j; }
                }
                indices |= static_cast<std::uint32_t>(bestIdx) << (i * 2);
            }
            out[12] = static_cast<std::uint8_t>(indices & 0xFF);
            out[13] = static_cast<std::uint8_t>((indices >> 8) & 0xFF);
            out[14] = static_cast<std::uint8_t>((indices >> 16) & 0xFF);
            out[15] = static_cast<std::uint8_t>((indices >> 24) & 0xFF);
        }
    } // anon namespace

    std::vector<std::uint8_t> encode_rgba_to_bc3(const std::uint8_t* rgba,
                                                  std::uint32_t width,
                                                  std::uint32_t height)
    {
        const auto bw = std::max(1u, (width + 3) / 4);
        const auto bh = std::max(1u, (height + 3) / 4);
        const auto pw = bw * 4;
        const auto ph = bh * 4;

        // Pad to 4-pixel boundary by replicating edges.
        std::vector<std::uint8_t> padded(static_cast<std::size_t>(pw) * ph * 4, 0);
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(padded.data() + y * pw * 4, rgba + y * width * 4, width * 4);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = width; x < pw; ++x)
                std::memcpy(padded.data() + (y * pw + x) * 4,
                            padded.data() + (y * pw + width - 1) * 4, 4);
        for (std::uint32_t y = height; y < ph; ++y)
            std::memcpy(padded.data() + y * pw * 4,
                        padded.data() + (height - 1) * pw * 4, pw * 4);

        std::vector<std::uint8_t> out(static_cast<std::size_t>(bw) * bh * 16);
        for (std::uint32_t by = 0; by < bh; ++by)
            for (std::uint32_t bx = 0; bx < bw; ++bx)
                encode_one_bc3_block(
                    padded.data() + (by * 4 * pw + bx * 4) * 4,
                    pw * 4,
                    out.data() + (by * bw + bx) * 16);
        return out;
    }

    // ── Bilinear RGBA resize ─────────────────────────────────────────────
    std::vector<std::uint8_t> resize_rgba(const std::uint8_t* src,
                                           std::uint32_t srcW, std::uint32_t srcH,
                                           std::uint32_t dstW, std::uint32_t dstH)
    {
        if (srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return {};
        std::vector<std::uint8_t> out(static_cast<std::size_t>(dstW) * dstH * 4);
        for (std::uint32_t y = 0; y < dstH; ++y)
        {
            const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(srcH) / static_cast<float>(dstH) - 0.5f;
            const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(sy), 0.0f, static_cast<float>(srcH - 1)));
            const auto y1 = std::min(srcH - 1, y0 + 1);
            const float ty = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
            for (std::uint32_t x = 0; x < dstW; ++x)
            {
                const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(srcW) / static_cast<float>(dstW) - 0.5f;
                const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(sx), 0.0f, static_cast<float>(srcW - 1)));
                const auto x1 = std::min(srcW - 1, x0 + 1);
                const float tx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);
                const auto dstOff = (static_cast<std::size_t>(y) * dstW + x) * 4;
                for (int c = 0; c < 4; ++c)
                {
                    const float c00 = src[(static_cast<std::size_t>(y0) * srcW + x0) * 4 + c];
                    const float c10 = src[(static_cast<std::size_t>(y0) * srcW + x1) * 4 + c];
                    const float c01 = src[(static_cast<std::size_t>(y1) * srcW + x0) * 4 + c];
                    const float c11 = src[(static_cast<std::size_t>(y1) * srcW + x1) * 4 + c];
                    const float top = std::lerp(c00, c10, tx);
                    const float bot = std::lerp(c01, c11, tx);
                    out[dstOff + c] = static_cast<std::uint8_t>(std::clamp(std::lerp(top, bot, ty), 0.0f, 255.0f));
                }
            }
        }
        return out;
    }

    // ── Full texture → BC3 normalisation ─────────────────────────────────
    void convert_texture_to_bc3(DdsTexture& texture,
                                std::uint32_t targetWidth,
                                std::uint32_t targetHeight,
                                std::uint32_t targetMipCount)
    {
        // Already BC3 at target size with enough mips?
        if (texture.valid && texture.compressed
            && texture.vkFormat == kVkFormatBc3UnormBlock
            && texture.width == targetWidth && texture.height == targetHeight
            && texture.mipData.size() >= targetMipCount)
        {
            texture.mipData.resize(targetMipCount);
            return;
        }

        // BC1 at target size → lossless convert, then trim mips.
        if (texture.valid && texture.compressed
            && texture.vkFormat == kVkFormatBc1RgbaUnormBlock
            && texture.width == targetWidth && texture.height == targetHeight
            && texture.mipData.size() >= targetMipCount)
        {
            convert_bc1_to_bc3(texture);
            texture.mipData.resize(targetMipCount);
            return;
        }

        // Everything else: decode to RGBA, resize, then encode BC3 mip chain.
        std::vector<std::uint8_t> rgba;
        if (texture.valid)
        {
            rgba = decode_texture_rgba(texture);
            if (rgba.empty() && !texture.rgba.empty())
                rgba = texture.rgba;
        }

        // Build fallback solid colour if we couldn't decode.
        if (rgba.empty() || rgba.size() < static_cast<std::size_t>(texture.width) * texture.height * 4)
        {
            const auto fallbackSize = static_cast<std::size_t>(targetWidth) * targetHeight * 4;
            rgba.resize(fallbackSize);
            for (std::size_t p = 0; p < static_cast<std::size_t>(targetWidth) * targetHeight; ++p)
            {
                rgba[p * 4 + 0] = 90;
                rgba[p * 4 + 1] = 130;
                rgba[p * 4 + 2] = 60;
                rgba[p * 4 + 3] = 255;
            }
            texture.width = targetWidth;
            texture.height = targetHeight;
        }

        // Resize to target dimensions if necessary.
        if (texture.width != targetWidth || texture.height != targetHeight)
        {
            rgba = resize_rgba(rgba.data(), texture.width, texture.height, targetWidth, targetHeight);
        }

        // Generate BC3 mip chain.
        texture.mipData.clear();
        texture.mipData.reserve(targetMipCount);
        auto mipW = targetWidth;
        auto mipH = targetHeight;
        std::vector<std::uint8_t> currentRgba = std::move(rgba);

        for (std::uint32_t mip = 0; mip < targetMipCount; ++mip)
        {
            texture.mipData.push_back(encode_rgba_to_bc3(currentRgba.data(), mipW, mipH));

            // Downsample for next mip (box filter).
            if (mip + 1 < targetMipCount)
            {
                const auto nextW = std::max(1u, mipW / 2);
                const auto nextH = std::max(1u, mipH / 2);
                std::vector<std::uint8_t> downsampled(static_cast<std::size_t>(nextW) * nextH * 4);
                for (std::uint32_t y = 0; y < nextH; ++y)
                {
                    for (std::uint32_t x = 0; x < nextW; ++x)
                    {
                        const auto sx = x * 2, sy = y * 2;
                        const auto sx1 = std::min(sx + 1, mipW - 1);
                        const auto sy1 = std::min(sy + 1, mipH - 1);
                        for (int c = 0; c < 4; ++c)
                        {
                            const unsigned v = currentRgba[(sy  * mipW + sx ) * 4 + c]
                                             + currentRgba[(sy  * mipW + sx1) * 4 + c]
                                             + currentRgba[(sy1 * mipW + sx ) * 4 + c]
                                             + currentRgba[(sy1 * mipW + sx1) * 4 + c];
                            downsampled[(y * nextW + x) * 4 + c] = static_cast<std::uint8_t>((v + 2) / 4);
                        }
                    }
                }
                currentRgba = std::move(downsampled);
                mipW = nextW;
                mipH = nextH;
            }
        }

        texture.width = targetWidth;
        texture.height = targetHeight;
        texture.vkFormat = kVkFormatBc3UnormBlock;
        texture.blockBytes = 16;
        texture.compressed = true;
        texture.valid = true;
        // Free RGBA data — we have mipData now.
        std::vector<std::uint8_t>().swap(texture.rgba);
    }
}
