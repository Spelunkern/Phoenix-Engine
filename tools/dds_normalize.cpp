// dds_normalize — one-shot data tool: normalise every .dds under a directory
// to the engine's canonical format (BC3/DXT5 with a full mip chain), using the
// exact same decode/resize/encode code the engine runs at load time. After a
// pass with this tool the engine's BC-native upload path activates and the
// runtime "Normalising textures to BC3" stage becomes a pure pass-through.
//
// Usage: dds_normalize <directory> <targetWidth> <targetHeight>
//   targetWidth/Height = 0 keeps each file's native dimensions (format+mips only).

#include "renderer/dds_loader.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    constexpr std::uint32_t kVkFormatBc3UnormBlock = 137;

    std::uint32_t mip_count_for(std::uint32_t width, std::uint32_t height)
    {
        // Same policy as the renderer: full chain trimmed to stop at 4x4 blocks.
        const auto maxDim = std::max(width, height);
        std::uint32_t fullMips = 1;
        for (auto d = maxDim; d > 1; d >>= 1)
            ++fullMips;
        const auto trimmed = fullMips > 2 ? fullMips - 2 : 1u;
        return std::max(1u, trimmed);
    }

    bool write_dds_bc3(const std::filesystem::path& path,
        std::uint32_t width, std::uint32_t height,
        const std::vector<std::vector<std::uint8_t>>& mips)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;

        std::uint8_t header[128]{};
        auto u32 = [&](std::size_t offset, std::uint32_t value) {
            std::memcpy(header + offset, &value, 4);
        };
        u32(0, 0x20534444u);                       // 'DDS '
        u32(4, 124);                               // header size
        // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE
        u32(8, 0x1u | 0x2u | 0x4u | 0x1000u | 0x20000u | 0x80000u);
        u32(12, height);
        u32(16, width);
        u32(20, static_cast<std::uint32_t>(mips.empty() ? 0 : mips[0].size())); // linear size
        u32(28, static_cast<std::uint32_t>(mips.size()));                       // mip count
        u32(76, 32);                               // pixel format size
        u32(80, 0x4u);                             // DDPF_FOURCC
        header[84] = 'D'; header[85] = 'X'; header[86] = 'T'; header[87] = '5';
        // DDSCAPS_TEXTURE | DDSCAPS_MIPMAP | DDSCAPS_COMPLEX
        u32(108, 0x1000u | 0x400000u | 0x8u);

        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& mip : mips)
            out.write(reinterpret_cast<const char*>(mip.data()), static_cast<std::streamsize>(mip.size()));
        return static_cast<bool>(out);
    }
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "Usage: dds_normalize <directory> <targetWidth> <targetHeight>\n");
        return 1;
    }

    const std::filesystem::path root = argv[1];
    const auto targetWidthArg = static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const auto targetHeightArg = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
    if (!std::filesystem::is_directory(root))
    {
        std::fprintf(stderr, "Not a directory: %s\n", root.string().c_str());
        return 1;
    }

    std::size_t converted = 0, skipped = 0, failed = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        if (!entry.is_regular_file())
            continue;
        auto extension = entry.path().extension().string();
        for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension != ".dds")
            continue;

        auto texture = phoenix::renderer::load_dds(entry.path());
        if (!texture.valid)
        {
            std::fprintf(stderr, "FAILED to load: %s\n", entry.path().string().c_str());
            ++failed;
            continue;
        }

        const auto targetWidth = targetWidthArg > 0 ? targetWidthArg : texture.width;
        const auto targetHeight = targetHeightArg > 0 ? targetHeightArg : texture.height;
        const auto targetMips = mip_count_for(targetWidth, targetHeight);

        // Already canonical? Skip (idempotent reruns).
        if (texture.compressed && texture.vkFormat == kVkFormatBc3UnormBlock
            && texture.width == targetWidth && texture.height == targetHeight
            && texture.mipData.size() >= targetMips)
        {
            ++skipped;
            continue;
        }

        phoenix::renderer::convert_texture_to_bc3(texture, targetWidth, targetHeight, targetMips);
        if (!texture.valid || texture.mipData.empty())
        {
            std::fprintf(stderr, "FAILED to convert: %s\n", entry.path().string().c_str());
            ++failed;
            continue;
        }

        if (!write_dds_bc3(entry.path(), targetWidth, targetHeight, texture.mipData))
        {
            std::fprintf(stderr, "FAILED to write: %s\n", entry.path().string().c_str());
            ++failed;
            continue;
        }
        ++converted;
    }

    std::printf("%s: converted %zu, already canonical %zu, failed %zu\n",
        root.string().c_str(), converted, skipped, failed);
    return failed == 0 ? 0 : 2;
}
