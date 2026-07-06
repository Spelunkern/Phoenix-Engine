#pragma once

#include "renderer/dds_loader.h"
#include "renderer/visibility_culling.h"
#include "renderer/opengl_renderer.h"
#include "world/character_loader.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phoenix::app { class LoadingScheduler; }

namespace phoenix::character
{
    struct MonsterCatalogEntry
    {
        std::size_t catalogIndex{};
        std::uint32_t monsterId{};
        std::uint32_t modelIndex{};
        std::uint32_t size{};
        std::string name;
        std::string label;
    };

    class MonsterManager
    {
    public:
        bool load_catalog(const std::filesystem::path& dataRoot);
        const std::vector<MonsterCatalogEntry>& catalog() const { return catalog_; }

        bool spawn(
            const std::filesystem::path& dataRoot,
            std::size_t catalogIndex,
            float x,
            float y,
            float z,
            float yaw,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::OpenGLRenderer& renderer);

        // Loads the monster spawn areas from a map's .svmap and queues them for
        // lazy streaming: an area only loads its model(s) and spawns its mobs once
        // it enters camera range. Each area can spawn several mobs of several ids;
        // mobs wander within their area. halfMap converts raw map-space X/Z into
        // engine/world space (0 for dungeons). Replaces any previously loaded set.
        bool load_map_monsters(
            const std::filesystem::path& dataRoot,
            const std::filesystem::path& svmapPath,
            float halfMap,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::OpenGLRenderer& renderer);

        std::size_t map_monster_total() const { return placements_.size(); }
        std::size_t map_monster_streamed() const { return streamedPlacements_; }

        // Optional terrain/floor height callback. When set, map mobs follow the
        // ground at spawn and while wandering (feet stay on the surface). Same
        // signature as the character system's sampler.
        using HeightSampler = float (*)(float worldX, float worldZ, void* userData);
        void set_height_sampler(HeightSampler fn, void* userData)
        {
            heightSampler_ = fn;
            heightSamplerCtx_ = userData;
        }

        void clear(phoenix::renderer::OpenGLRenderer& renderer);
        // Removes only manually-spawned monsters, leaving the map's .svmap mobs
        // intact. Used by the panel's "clear" command.
        void clear_manual(phoenix::renderer::OpenGLRenderer& renderer);
        // workerPool (optional): parallelizes per-entity skinning across the
        // shared CPU pool. Serial fallback when null or few entities.
        void update(
            float deltaSeconds,
            const phoenix::renderer::CameraView& view,
            phoenix::renderer::OpenGLRenderer& renderer,
            phoenix::app::LoadingScheduler* workerPool = nullptr);

        // World-space floating name label for one on-screen monster (no type).
        struct MonsterLabel
        {
            float x{};
            float y{};
            float z{};
            const std::string* name{};
        };
        const std::vector<MonsterLabel>& labels() const { return labels_; }

        std::size_t active_count() const { return active_.size(); }
        std::size_t visible_count() const { return visibleCount_; }
        bool active() const { return !active_.empty(); }
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

        // Per-vertex skinning inputs, accumulated locally during a load only to
        // build the static skinning plan (referenced bones); not stored per visual.
        struct SourceVertex
        {
            float weights[3]{};
            std::uint8_t bones[3]{};
            std::uint32_t meshBoneBase{};
            std::uint32_t meshBoneCount{};
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
            // GPU-skinning bind mesh: unscaled bind pose + global bone indices
            // and weights. Skinned on the GPU from the per-entity bone palette.
            std::vector<phoenix::renderer::SkinnedVertex> skinnedBind;
            std::vector<phoenix::world::CharacterBone> meshBones;
            // Static skinning plan: unique referenced mesh bones, each with the
            // animation bone it reads and its precomputed (transposed) bind matrix.
            // Skinning touches ~dozens of bones instead of iterating every vertex.
            std::vector<std::uint32_t> paletteGlobalBone;   // palette slot (mesh bone)
            std::vector<std::uint32_t> paletteLocalBone;    // animation bone index
            std::vector<float> paletteMeshBind;             // 16 floats / entry
            std::vector<std::uint32_t> indices;
            std::vector<phoenix::renderer::ObjectBatch> batches;
            std::vector<AnimationChoice> animations;
            std::size_t breathAnimation{};
            std::size_t idleAnimation{};
            std::size_t walkAnimation{};   // wandering mobs (slow move)
            std::size_t runAnimation{};    // wandering mobs (occasional fast move)
            float boundsCenterY{ 1.0f };
            float boundsRadius{ 1.0f };
            float localGroundY{};
            float modelTopY{ 2.0f };   // scaled-local max Y; head anchor for labels
            bool ready{};
            // Texture slots this visual references (one per part); used to
            // refcount/free slots when the visual is evicted.
            std::vector<std::uint32_t> textureSlots;
            // Textures decoded off-thread, awaiting GPU upload on the main thread
            // (slot, pixels). Cleared by finalize_visual once uploaded.
            std::vector<std::pair<std::uint32_t, phoenix::renderer::DdsTexture>> pendingTextureUploads;
        };

        // Resolved inputs for one model's visual: produced on the main thread
        // (path resolution + texture-slot assignment), consumed by the worker
        // parse. Holds no engine/shared state, so it is safe across threads.
        struct VisualPartLoad
        {
            std::filesystem::path meshPath;
            std::filesystem::path texturePath;
            std::uint32_t textureSlot{};
            bool decodeTexture{};
        };
        struct VisualLoadPlan
        {
            std::uint32_t modelIndex{};
            std::uint32_t textureBaseSlot{};
            std::vector<VisualPartLoad> parts;
            std::filesystem::path animationRoot;
            std::string walkAni;
            std::string runAni;
            std::string breathAni;
            std::string idleAni;
            std::string attack1Ani;
        };

        struct ActiveMonster
        {
            std::uint32_t modelIndex{};
            std::size_t catalogIndex{};   // -> catalog_ for the name label
            std::size_t placementIndex{ static_cast<std::size_t>(-1) };  // -> placements_ (map mobs)
            float x{};
            float y{};
            float z{};
            float yaw{};
            float scale{ 1.0f };
            bool fromMap{};   // true = loaded from .svmap; false = manual spawn
            // Wander state (map mobs only). A mob roams its spawn-area box: rests,
            // then occasionally walks to a random point in the box, and very
            // occasionally runs a short distance instead. y stays at spawn height.
            bool wander{};
            bool wanderMoving{};
            bool wanderRunning{};
            float areaMinX{};
            float areaMaxX{};
            float areaMinZ{};
            float areaMaxZ{};
            float targetX{};
            float targetZ{};
            float wanderWaitTimer{};
            float wanderWaitInterval{};
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
        };

        Visual* load_visual(
            const std::filesystem::path& dataRoot,
            const MonsterCatalogEntry& entry,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::OpenGLRenderer& renderer);
        // Main thread: resolve paths + assign texture slots for a model.
        bool plan_visual(
            const std::filesystem::path& dataRoot,
            const MonsterCatalogEntry& entry,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            VisualLoadPlan& out);
        // Worker thread: heavy load (3DC/DDS/ANI), pure. nullptr on failure.
        static std::shared_ptr<Visual> parse_visual(const VisualLoadPlan& plan);
        // Main thread: upload the parsed visual's textures and store it ready.
        Visual* finalize_visual(
            std::uint32_t modelIndex,
            std::shared_ptr<Visual> visual,
            std::uint32_t textureBaseSlot,
            phoenix::renderer::OpenGLRenderer& renderer);
        // Promotes one finished async visual load to ready per call.
        bool pump_visual_loads(phoenix::renderer::OpenGLRenderer& renderer);
        // Spawns areas that have entered camera range (loading visuals async).
        void stream_map_monsters(
            const phoenix::renderer::CameraView& view,
            phoenix::renderer::OpenGLRenderer& renderer,
            phoenix::app::LoadingScheduler* workerPool);
        void rebuild_render_mesh(phoenix::renderer::OpenGLRenderer& renderer);
        // Advances a wandering mob: rests, then walks/runs to a random in-area
        // target, updating position/yaw. Sets wanderMoving/wanderRunning, which
        // drive the move-vs-idle and walk-vs-run choices in update_animation.
        void update_wander(float deltaSeconds, ActiveMonster& monster, const Visual& visual);
        void update_animation(float deltaSeconds, ActiveMonster& monster, const Visual& visual);
        // Removes map mobs whose spawn area moved out of range (and the farthest
        // over a hard budget), resetting placements so they re-stream when near.
        void despawn_distant(const phoenix::renderer::CameraView& view);
        // Under texture-slot pressure, evicts the least-recently-used visuals that
        // no active mob references, freeing their texture slots for reuse.
        void evict_visuals_if_needed(phoenix::renderer::OpenGLRenderer& renderer);
        void evict_visual(std::uint32_t modelIndex);

        // One unique pose to skin this frame (a snapshot of an entity's animation
        // state). Written into paletteFloats_[baseBone*16 ..]; entities sharing a
        // pose share a job, so the heavy skinning fans out across the worker pool.
        struct PaletteJob
        {
            const Visual* visual{};
            std::uint32_t baseBone{};
            std::size_t activeAnimation{};
            float animationSeconds{};
            bool blend{};
            std::size_t previousAnimation{};
            float previousAnimationSeconds{};
            float animationBlendSeconds{};
            float animationBlendDuration{};
            bool previousAnimationHoldEnd{};
        };
        // Writes one job's bone palette into paletteFloats_ at job.baseBone.
        // Thread-safe for disjoint regions (thread-local scratch).
        void write_palette_at(const PaletteJob& job);

        // A map (.svmap) monster spawn area awaiting (or after) lazy streaming.
        // Box is engine/world space; groundY is the authored spawn height. When
        // streamed, `count` mobs of this kind spawn at random points in the box,
        // each wandering within it. `spawned` flips once turned into mobs.
        struct MapSpawn
        {
            std::size_t catalogIndex{};
            std::uint32_t modelIndex{};
            float minX{};
            float maxX{};
            float minZ{};
            float maxZ{};
            float groundY{};
            std::uint32_t count{ 1 };
            bool spawned{};
        };

        std::vector<MonsterCatalogEntry> catalog_;
        // mobId -> catalogIndex, for resolving svmap monster areas.
        std::unordered_map<std::uint32_t, std::size_t> catalogByMobId_;
        std::unordered_map<std::uint32_t, std::vector<VisualPartRow>> visualRows_;
        std::unordered_map<std::uint32_t, Visual> visuals_;
        // In-flight async visual loads (modelIndex -> future); failed ones noted.
        std::unordered_map<std::uint32_t, std::future<std::shared_ptr<Visual>>> visualLoads_;
        std::unordered_set<std::uint32_t> failedModels_;
        std::unordered_map<std::string, std::uint32_t> textureSlotByPath_;
        // Texture-slot lifetime (shared across visuals; freed when the last user
        // is evicted). textureKeyBySlot_ is the inverse of textureSlotByPath_.
        std::unordered_map<std::uint32_t, std::uint32_t> slotRefcount_;
        std::unordered_map<std::uint32_t, std::string> textureKeyBySlot_;
        std::vector<std::uint32_t> freeTextureSlots_;
        std::unordered_map<std::uint32_t, std::uint32_t> modelLastUsedFrame_;   // LRU
        std::filesystem::path catalogRoot_;
        std::vector<MapSpawn> placements_;
        std::filesystem::path mapDataRoot_;
        std::uint32_t mapTextureBaseSlot_{};
        std::uint32_t mapTextureReserve_{};
        std::size_t streamedPlacements_{};
        HeightSampler heightSampler_{};
        void* heightSamplerCtx_{};
        std::mt19937 wanderRng_{ 0x51ED270Bu };
        std::vector<MonsterLabel> labels_;
        std::vector<ActiveMonster> active_;
        std::vector<phoenix::renderer::SkinnedVertex> renderVertices_;
        std::vector<std::uint32_t> renderIndices_;
        // One shared bind mesh per model: modelIndex -> its first index in
        // renderIndices_. All entities of a model draw this geometry, instanced.
        std::unordered_map<std::uint32_t, std::uint32_t> modelIndexOffset_;
        std::vector<float> paletteFloats_;   // per-frame concatenated bone palettes
        std::vector<phoenix::renderer::ObjectInstance> instances_;
        std::vector<phoenix::renderer::ObjectBatch> instanceBatches_;
        std::uint32_t nextTextureSlot_{};
        std::uint32_t frameCounter_{};   // frame stamp for visual-eviction LRU
        std::size_t visibleCount_{};
        std::size_t lastStatusVisible_{ static_cast<std::size_t>(-1) };
        std::string status_;
    };
}
