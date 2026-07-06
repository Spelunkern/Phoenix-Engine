#include "renderer/png_loader.h"

#include "assets/data_index.h"

// PNG-only decode is all this file needs; keep the rest of stb_image's format
// detectors out of the build. STBI_NO_STDIO because files are read through
// read_file_binary (case-insensitive fallback, same as every other asset
// loader in this codebase) and decoded from memory instead.
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace phoenix::renderer
{
    PngImage load_png(const std::filesystem::path& path)
    {
        PngImage image;

        const auto fileData = phoenix::assets::read_file_binary(path);
        if (fileData.empty())
            return image;

        int width = 0, height = 0, sourceChannels = 0;
        stbi_uc* pixels = stbi_load_from_memory(fileData.data(), static_cast<int>(fileData.size()),
            &width, &height, &sourceChannels, 4);
        if (!pixels)
            return image;

        image.width = static_cast<std::uint32_t>(width);
        image.height = static_cast<std::uint32_t>(height);
        image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
        stbi_image_free(pixels);
        image.valid = true;
        return image;
    }
}
