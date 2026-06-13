#pragma once

#include "renderer/dds_loader.h"
#include "renderer/visibility_culling.h"
#include "renderer/vulkan_renderer.h"
#include "world/character_loader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::app { class LoadingScheduler; }

namespace phoenix::character
{
    struct NpcCatalogEntry
    {
        std::size_t catalogIndex{};
        std::uint32_t npcIndex{};
        std::string npcId;
        std::uint32_t npcType{};
        std::string npcTypeName;
        std::uint32_t modelIndex{};
        std::string name;
        std::string label;
    };

    class NpcManager
    {
    public:
        bool load_catalog(const std::filesystem::path& dataRoot);
        const std::vector<NpcCatalogEntry>& catalog() const { return catalog_; }

        bool spawn(
            const std::filesystem::path& dataRoot,
            std::size_t catalogIndex,
            float x,
            float y,
            float z,
            float yaw,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::VulkanRenderer& renderer);

        void clear(phoenix::renderer::VulkanRenderer& renderer);
        // workerPool (optional): parallelizes per-entity skinning across the
        // shared CPU pool. Serial fallback when null or few entities.
        void update(
            float deltaSeconds,
            const phoenix::renderer::CameraView& view,
            phoenix::renderer::VulkanRenderer& renderer,
            phoenix::app::LoadingScheduler* workerPool = nullptr);

        std::size_t active_count() const { return active_.size(); }
        std::size_t visible_count() const { return visibleCount_; }
        bool active() const { return !active_.empty(); }
        const std::string& active_label() const { return activeLabel_; }
        const std::string& status() const { return status_; }

    private:
        struct VisualPartRow
        {
            std::uint32_t modelIndex{};
            std::uint32_t objectIndex{};
            std::string meshName;
            std::string textureName;
            std::string walkAni;
            std::string runAni;
            std::string attack1Ani;
            std::string attack2Ani;
            std::string attack3Ani;
            std::string deathAni;
            std::string breathAni;
            std::string damageAni;
            std::string idleAni;
        };

        struct SourceVertex
        {
            float position[3]{};
            float normal[3]{};
            float uv[2]{};
            float weights[3]{};
            std::uint8_t bones[3]{};
            std::uint32_t meshBoneBase{};
            std::uint32_t meshBoneCount{};
            std::uint32_t gpuIndex{};
        };

        struct AnimationChoice
        {
            std::string name;
            std::filesystem::path path;
            phoenix::world::CharacterAnimation animation;
            // Bind-relative local matrices (16 floats/bone), precomputed once at
            // load. Recomputing these — and the per-bone parent inverse — every
            // frame was the dominant skinning cost; they never change, so they
            // are cached here and reused by compute_client_finals.
            std::vector<float> bindLocals;
        };

        struct Visual
        {
            std::vector<phoenix::renderer::TerrainVertex> bindVertices;
            // GPU-skinning bind mesh: unscaled bind pose + global bone indices
            // and weights. Skinned on the GPU from the per-entity bone palette.
            std::vector<phoenix::renderer::SkinnedVertex> skinnedBind;
            std::vector<SourceVertex> sourceVertices;
            std::vector<phoenix::world::CharacterBone> meshBones;
            std::vector<std::uint32_t> indices;
            std::vector<phoenix::renderer::ObjectBatch> batches;
            std::vector<std::filesystem::path> texturePaths;
            std::vector<AnimationChoice> animations;
            std::size_t breathAnimation{};
            std::size_t idleAnimation{};
            float boundsCenterY{ 1.0f };
            float boundsRadius{ 1.0f };
            float localGroundY{};
            bool ready{};
        };

        struct ActiveNpc
        {
            std::uint32_t modelIndex{};
            std::uint32_t vertexOffset{};
            std::uint32_t vertexCount{};
            std::uint32_t indexOffset{};
            float x{};
            float y{};
            float z{};
            float yaw{};
            std::size_t activeAnimation{};
            std::size_t previousAnimation{};
            float animationSeconds{};
            float previousAnimationSeconds{};
            float animationBlendSeconds{};
            float animationBlendDuration{};
            bool previousAnimationHoldEnd{};
            bool pendingHoldPreviousAtEnd{};
            bool playingIdle{};
            float idleGestureTimer{};
            std::uint32_t idleGesturePick{};
            bool visible{};
            bool everSkinned{};   // first visible frame always skins (no LOD skip)
        };

        Visual* load_visual(
            const std::filesystem::path& dataRoot,
            const NpcCatalogEntry& entry,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::VulkanRenderer& renderer);
        void rebuild_render_mesh(phoenix::renderer::VulkanRenderer& renderer);
        void update_animation(float deltaSeconds, ActiveNpc& npc, const Visual& visual);
        // Computes the NPC's model-space bone palette (mesh-bind * animated final
        // per bone) and appends it, flattened to 16 floats/bone, to paletteFloats_.
        void append_palette(const ActiveNpc& npc, const Visual& visual);

        std::vector<NpcCatalogEntry> catalog_;
        std::unordered_map<std::uint32_t, std::vector<VisualPartRow>> visualRows_;
        std::unordered_map<std::uint32_t, Visual> visuals_;
        std::unordered_map<std::string, std::uint32_t> textureSlotByPath_;
        std::filesystem::path catalogRoot_;
        std::vector<ActiveNpc> active_;
        std::vector<phoenix::renderer::SkinnedVertex> renderVertices_;
        std::vector<std::uint32_t> renderIndices_;
        std::vector<float> paletteFloats_;   // per-frame concatenated bone palettes
        std::vector<phoenix::renderer::ObjectInstance> instances_;
        std::vector<phoenix::renderer::ObjectBatch> instanceBatches_;
        std::uint32_t nextTextureSlot_{};
        std::uint32_t frameCounter_{};   // drives staggered animation LOD
        std::size_t visibleCount_{};
        std::size_t lastStatusVisible_{ static_cast<std::size_t>(-1) };
        std::string activeLabel_;
        std::string status_;
    };
}
