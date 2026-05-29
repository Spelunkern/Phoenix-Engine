#pragma once

#include "assets/data_index.h"
#include "renderer/vulkan_renderer.h"
#include "world/character_loader.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::world
{
    struct ActorSourceVertex
    {
        float position[3]{};
        float normal[3]{};
        float weights[3]{};
        std::uint8_t bones[3]{};
        std::uint32_t meshBoneBase{};
        std::uint32_t meshBoneCount{};
        float outputScale{ 1.0f };
    };

    // GPU-friendly source vertex for compute skinning (48 bytes, std430 aligned).
    struct GpuSkinSourceVertex
    {
        float posX{}, posY{}, posZ{};
        float nrmX{}, nrmY{}, nrmZ{};
        float weight0{}, weight1{}, weight2{};
        std::uint32_t packedBones{};   // bones[0] | (bones[1]<<8) | (bones[2]<<16)
        std::uint32_t meshBoneBase{};
        float outputScale{ 1.0f };
    };

    struct ActorSkinData
    {
        std::vector<ActorSourceVertex> sourceVertices;
        std::vector<CharacterBone> meshBones;
        // For each mesh bone: the animation bone index it maps to.
        // Enables eager computation of all skin matrices on CPU for GPU upload.
        std::vector<std::uint8_t> meshBoneAnimIndex;
    };

    struct ActorAnimationSet
    {
        CharacterAnimation breath;
        CharacterAnimation idle;
        CharacterAnimation walk;
        CharacterAnimation run;
    };

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
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> frames; // world VANI only
            ActorSkinData skinData;
            ActorAnimationSet animations;
            float worldX{};
            float worldY{};
            float worldZ{};
            float boundingRadius{ 48.0f };
            bool isMob{};
            bool hasActorSkin{};
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

    void skin_actor_vertices(
        const ActorSkinData& skin,
        std::span<phoenix::renderer::TerrainVertex> vertices,
        const CharacterAnimation& animation,
        float frame);

    // Compute all skin matrices for an actor at a given animation frame.
    // Output: 16 floats (column-major mat4) per mesh bone, appended to outMatrices.
    // Returns the number of matrices written.
    std::uint32_t compute_skin_matrices(
        const ActorSkinData& skin,
        const CharacterAnimation& animation,
        float frame,
        std::vector<float>& outMatrices);

    // Build GPU-friendly source vertex array from ActorSkinData.
    std::vector<GpuSkinSourceVertex> build_gpu_skin_sources(const ActorSkinData& skin);

    ActorScene build_actor_scene(
        const std::filesystem::path& dataRoot,
        const std::string& mapStem,
        const phoenix::assets::DataIndex& assets,
        std::uint32_t textureLayerBase,
        float (*heightSampler)(float worldX, float worldZ, void* userData),
        void* heightUserData,
        bool isDungeon = false);
}
