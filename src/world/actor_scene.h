#pragma once

#include "assets/data_index.h"
#include "renderer/vulkan_renderer.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::world
{
    struct ActorScene
    {
        struct Label
        {
            std::string text;
            float x{};
            float y{};
            float z{};
            float radius{};
            float offsetY{};
            std::uint32_t animatedInstanceIndex{ UINT32_MAX };
            bool followsAnimatedInstance{};
        };

        struct BatchBounds
        {
            float x{};
            float y{};
            float z{};
            float radius{};
        };

        struct VertexAnimation
        {
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> frames;      // breath
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> idleFrames;   // idle gesture
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> walkFrames;   // walk cycle (mobs only)
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> runFrames;    // run cycle (mobs only)
            float worldX{};
            float worldY{};
            float worldZ{};
            float boundingRadius{ 48.0f };
            bool isMob{}; // true for monsters (they roam), false for NPCs (stationary)
        };

        std::vector<phoenix::renderer::TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<phoenix::renderer::ObjectInstance> instances;
        std::vector<phoenix::renderer::ObjectBatch> batches;
        std::vector<BatchBounds> batchBounds;
        std::vector<phoenix::renderer::TerrainVertex> animatedVertices;
        std::vector<std::uint32_t> animatedIndices;
        std::vector<phoenix::renderer::ObjectInstance> animatedBaseInstances;
        std::vector<phoenix::renderer::ObjectInstance> animatedInstances;
        std::vector<phoenix::renderer::ObjectBatch> animatedBatches;
        std::vector<BatchBounds> animatedBatchBounds;
        std::vector<VertexAnimation> vertexAnimations;
        std::vector<std::filesystem::path> texturePaths;
        std::vector<Label> labels;
        std::uint32_t npcCount{};
        std::uint32_t monsterCount{};
    };

    ActorScene build_actor_scene(
        const std::filesystem::path& dataRoot,
        const std::string& mapStem,
        const phoenix::assets::DataIndex& assets,
        std::uint32_t textureLayerBase,
        float (*heightSampler)(float worldX, float worldZ, void* userData),
        void* heightUserData);
}
