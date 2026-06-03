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
        std::uint32_t blockBytes{};
        bool compressed{};
        std::vector<std::vector<std::uint8_t>> mipData;
        std::vector<std::uint8_t> rgba;
        bool valid{};
    };

    DdsTexture load_dds(const std::filesystem::path& path);
    std::vector<std::uint8_t> decode_texture_rgba(const DdsTexture& texture);

    /// Convert a BC1 texture's mip chain to BC3 in-place (lossless, adds opaque alpha blocks).
    void convert_bc1_to_bc3(DdsTexture& texture);

    /// Encode an RGBA buffer to a single BC3 mip level.  Basic quality encoder.
    std::vector<std::uint8_t> encode_rgba_to_bc3(const std::uint8_t* rgba,
                                                  std::uint32_t width,
                                                  std::uint32_t height);

    /// Bilinear-resize an RGBA image.  Returns empty vector on failure.
    std::vector<std::uint8_t> resize_rgba(const std::uint8_t* src,
                                           std::uint32_t srcW, std::uint32_t srcH,
                                           std::uint32_t dstW, std::uint32_t dstH);

    /// Returns true if the texture has meaningful alpha variation (some pixels with
    /// alpha well below opaque). Used to auto-detect cutout/transparency assets so
    /// the renderer can apply alpha-test instead of guessing by filename.
    bool texture_has_alpha_cutout(const DdsTexture& texture);

    /// Normalise a single texture to BC3 at target dimensions with a full mip chain.
    /// Handles BC1, BC2, BC3, and uncompressed textures.  Invalid textures are filled
    /// with a solid-colour fallback.
    void convert_texture_to_bc3(DdsTexture& texture,
                                std::uint32_t targetWidth,
                                std::uint32_t targetHeight,
                                std::uint32_t targetMipCount);
}
