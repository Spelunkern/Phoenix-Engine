#include "app/water_resources.h"

#include <algorithm>
#include <cstdint>
#include <iterator>

namespace phoenix::app
{
    WaterResourceLayout make_water_resource_layout(
        std::size_t assetTextureLayerBase,
        std::size_t assetTextureCount,
        std::uint32_t animationFrameCount)
    {
        WaterResourceLayout layout{};
        layout.frameBase = assetTextureLayerBase + assetTextureCount;
        layout.firstFreeLayer = layout.frameBase + animationFrameCount;
        return layout;
    }

    void append_water_texture_jobs(
        std::vector<TextureLoadJob>& jobs,
        const std::filesystem::path& stillTexturePath,
        const phoenix::runtime::WaterAnimation& animation,
        const WaterResourceLayout& layout)
    {
        if (!stillTexturePath.empty())
            jobs.push_back({ layout.stillLayer, stillTexturePath });

        for (std::uint32_t i = 0; i < animation.frameCount; ++i)
            jobs.push_back({ layout.frameBase + i, animation.framePaths[i] });
    }

    void build_water_mesh(
        float mapSize,
        std::vector<phoenix::renderer::TerrainVertex>& vertices,
        std::vector<std::uint32_t>& indices)
    {
        vertices.clear();
        indices.clear();

        mapSize = std::max(1.0f, mapSize);
        const float halfMap = mapSize * 0.5f;
        const float waterColor[3]{ 0.02f, 0.32f, 0.78f };
        const float points[4][3]{
            { -halfMap, phoenix::world::kWaterSurfaceY, -halfMap },
            { halfMap, phoenix::world::kWaterSurfaceY, -halfMap },
            { -halfMap, phoenix::world::kWaterSurfaceY, halfMap },
            { halfMap, phoenix::world::kWaterSurfaceY, halfMap },
        };
        const float uvs[4][2]{
            { 0.0f, 0.0f },
            { mapSize / 64.0f, 0.0f },
            { 0.0f, mapSize / 64.0f },
            { mapSize / 64.0f, mapSize / 64.0f },
        };

        vertices.reserve(4);
        for (std::size_t i = 0; i < std::size(points); ++i)
        {
            phoenix::renderer::TerrainVertex vertex{};
            vertex.position[0] = points[i][0];
            vertex.position[1] = points[i][1];
            vertex.position[2] = points[i][2];
            vertex.color[0] = waterColor[0];
            vertex.color[1] = waterColor[1];
            vertex.color[2] = waterColor[2];
            vertex.normal[1] = 1.0f;
            vertex.uv[0] = uvs[i][0];
            vertex.uv[1] = uvs[i][1];
            vertex.textureLayer = phoenix::world::kWaterStillTextureLayer;
            vertices.push_back(vertex);
        }

        indices = { 0, 2, 1, 1, 2, 3 };
    }

    void configure_water_renderer(
        phoenix::renderer::VulkanRenderer& renderer,
        const std::filesystem::path& stillTexturePath,
        const phoenix::runtime::WaterAnimation& animation,
        const WaterResourceLayout& layout)
    {
        renderer.set_water_layer(
            !stillTexturePath.empty() ? static_cast<std::uint32_t>(layout.stillLayer) : UINT32_MAX);

        if (animation.frameCount > 0)
        {
            renderer.set_water_animation(
                static_cast<std::uint32_t>(layout.frameBase),
                animation.frameCount,
                animation.tileSize);
        }
    }
}
