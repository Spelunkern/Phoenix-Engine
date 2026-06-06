#pragma once

#include "renderer/vulkan_renderer.h"
#include "runtime/phoenix_runtime.h"
#include "world/water_constants.h"

#include <cstddef>
#include <filesystem>
#include <vector>

namespace phoenix::app
{
    struct TextureLoadJob
    {
        std::size_t slot{};
        std::filesystem::path path;
    };

    struct WaterResourceLayout
    {
        std::size_t stillLayer{ phoenix::world::kWaterStillTextureLayer };
        std::size_t frameBase{};
        std::size_t firstFreeLayer{};
    };

    WaterResourceLayout make_water_resource_layout(
        std::size_t assetTextureLayerBase,
        std::size_t assetTextureCount,
        std::uint32_t animationFrameCount);

    void append_water_texture_jobs(
        std::vector<TextureLoadJob>& jobs,
        const std::filesystem::path& stillTexturePath,
        const phoenix::runtime::WaterAnimation& animation,
        const WaterResourceLayout& layout);

    void build_water_mesh(
        float mapSize,
        std::vector<phoenix::renderer::TerrainVertex>& vertices,
        std::vector<std::uint32_t>& indices);

    void configure_water_renderer(
        phoenix::renderer::VulkanRenderer& renderer,
        const std::filesystem::path& stillTexturePath,
        const phoenix::runtime::WaterAnimation& animation,
        const WaterResourceLayout& layout);
}
