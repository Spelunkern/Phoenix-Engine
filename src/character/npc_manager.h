#pragma once

#include "renderer/dds_loader.h"
#include "renderer/visibility_culling.h"
#include "renderer/vulkan_renderer.h"
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
    struct NpcCatalogEntry
    {
        std::size_t catalogIndex{};
        std::uint32_t npcIndex{};
        std::string npcId;
        std::uint32_t npcType{};
        std::uint32_t npcTypeId{};
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

        // Loads the NPC placements authored in a map's .svmap and queues them for
        // lazy streaming: each placement only loads its model and starts
        // consuming CPU/GPU once it enters camera range (see update()). svmap
        // NPCs keep their authored Y exactly — they are NOT clamped to terrain.
        // svmapPath is the map's .svmap; halfMap converts raw map-space X/Z into
        // engine/world space (0 for dungeons). Replaces any previously loaded set.
        bool load_map_npcs(
            const std::filesystem::path& dataRoot,
            const std::filesystem::path& svmapPath,
            float halfMap,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::VulkanRenderer& renderer);

        std::size_t map_npc_total() const { return placements_.size(); }
        std::size_t map_npc_streamed() const { return streamedPlacements_; }

        void clear(phoenix::renderer::VulkanRenderer& renderer);
        // Removes only manually-spawned NPCs, leaving the map's .svmap NPCs (which
        // are always present) intact. Used by the panel's "clear" command.
        void clear_manual(phoenix::renderer::VulkanRenderer& renderer);
        // workerPool (optional): parallelizes per-entity skinning across the
        // shared CPU pool. Serial fallback when null or few entities.
        void update(
            float deltaSeconds,
            const phoenix::renderer::CameraView& view,
            phoenix::renderer::VulkanRenderer& renderer,
            phoenix::app::LoadingScheduler* workerPool = nullptr);

        // World-space floating label for one on-screen NPC: the head anchor and
        // pointers into the catalog (stable for the map's lifetime).
        struct NpcLabel
        {
            float x{};
            float y{};
            float z{};
            const std::string* name{};
            const std::string* typeName{};
        };
        // Visible NPCs' name/type labels, refreshed each update(). The UI projects
        // these to screen and draws them (name yellow, type light blue).
        const std::vector<NpcLabel>& labels() const { return labels_; }

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
            // Static skinning plan: the unique mesh bones any vertex references,
            // each with the animation bone it reads and its precomputed
            // (transposed) bind matrix. Lets per-frame skinning touch ~dozens of
            // bones instead of iterating every vertex (the reference set never
            // changes). Parallel arrays, one entry per referenced bone.
            std::vector<std::uint32_t> paletteGlobalBone;   // palette slot (mesh bone)
            std::vector<std::uint32_t> paletteLocalBone;    // animation bone index
            std::vector<float> paletteMeshBind;             // 16 floats / entry
            std::vector<std::uint32_t> indices;
            std::vector<phoenix::renderer::ObjectBatch> batches;
            std::vector<AnimationChoice> animations;
            std::size_t breathAnimation{};
            std::size_t idleAnimation{};
            std::size_t walkAnimation{};   // used by patrolling NPCs while moving
            float boundsCenterY{ 1.0f };
            float boundsRadius{ 1.0f };
            float localGroundY{};
            float modelTopY{ 2.0f };   // scaled-local max Y; head anchor for labels
            bool ready{};
            // Texture slots this visual references (one per part). Used to
            // refcount/free slots when the visual is evicted.
            std::vector<std::uint32_t> textureSlots;
            // Textures decoded off-thread, awaiting GPU upload on the main thread
            // (slot, pixels). Cleared by finalize_visual once uploaded.
            std::vector<std::pair<std::uint32_t, phoenix::renderer::DdsTexture>> pendingTextureUploads;
        };

        // Resolved inputs for one model's visual, produced on the main thread
        // (path resolution + texture-slot assignment) and consumed by the
        // worker-thread parse. Holds no engine/shared state so it is safe to
        // hand to another thread.
        struct VisualPartLoad
        {
            std::filesystem::path meshPath;
            std::filesystem::path texturePath;
            std::uint32_t textureSlot{};
            bool decodeTexture{};   // newly allocated slot — decode + upload it
        };
        struct VisualLoadPlan
        {
            std::uint32_t modelIndex{};
            std::uint32_t textureBaseSlot{};
            std::vector<VisualPartLoad> parts;
            std::filesystem::path animationRoot;
            std::string walkAni;
            std::string breathAni;
            std::string idleAni;
            std::string runAni;
            std::string attack1Ani;
        };

        // One authored stop on an NPC's path, in engine/world space. For a
        // patrol the y is already localGroundY-adjusted (render origin); for a
        // placement waypoint it is the raw authored ground Y (see MapPlacement).
        struct Waypoint
        {
            float x{};
            float y{};
            float z{};
            float yaw{};
        };

        struct ActiveNpc
        {
            std::uint32_t modelIndex{};
            std::size_t catalogIndex{};   // -> catalog_ for name/type labels
            std::size_t placementIndex{ static_cast<std::size_t>(-1) };  // -> placements_ (map NPCs)
            float x{};
            float y{};
            float z{};
            float yaw{};
            // Patrol state (empty/false for static NPCs). A patrol NPC walks
            // between its waypoints infrequently; in between it idles like any
            // static NPC. Waypoints are render-space (y localGroundY-adjusted).
            bool isPatrol{};
            bool patrolMoving{};
            std::uint32_t patrolFrom{};   // waypoint the NPC currently rests at
            std::uint32_t patrolTo{};     // waypoint being walked toward
            float patrolWaitTimer{};      // counts up while resting
            float patrolWaitInterval{};   // randomized rest duration before next move
            std::vector<Waypoint> patrol;
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
            bool fromMap{};       // true = loaded from .svmap; false = manual spawn
        };

        Visual* load_visual(
            const std::filesystem::path& dataRoot,
            const NpcCatalogEntry& entry,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            phoenix::renderer::VulkanRenderer& renderer);
        // Main thread: resolves paths and assigns texture slots for a model.
        // Returns false if the model has no rows or the texture reserve is full.
        bool plan_visual(
            const std::filesystem::path& dataRoot,
            const NpcCatalogEntry& entry,
            std::uint32_t textureBaseSlot,
            std::uint32_t textureSlotReserve,
            VisualLoadPlan& out);
        // Worker thread: the heavy load (3DC meshes, DDS decode, ANI parse). Pure
        // — no engine/shared state — so it can run off the render thread. Returns
        // a not-yet-ready Visual (textures pending upload), or nullptr on failure.
        static std::shared_ptr<Visual> parse_visual(const VisualLoadPlan& plan);
        // Main thread: uploads the parsed visual's textures and stores it ready.
        Visual* finalize_visual(
            std::uint32_t modelIndex,
            std::shared_ptr<Visual> visual,
            std::uint32_t textureBaseSlot,
            phoenix::renderer::VulkanRenderer& renderer);
        void rebuild_render_mesh(phoenix::renderer::VulkanRenderer& renderer);
        // Spawns any not-yet-streamed map placements that have entered camera
        // range. Visuals load asynchronously on workerPool (when provided) so the
        // first sighting of a new NPC type doesn't hitch the render thread; the
        // NPC appears a moment later once its visual is ready. Runs in update().
        void stream_map_npcs(
            const phoenix::renderer::CameraView& view,
            phoenix::renderer::VulkanRenderer& renderer,
            phoenix::app::LoadingScheduler* workerPool);
        // Promotes any finished async visual loads to ready (main-thread GPU
        // upload). Returns true if a new model became ready this call.
        bool pump_visual_loads(phoenix::renderer::VulkanRenderer& renderer);
        void update_animation(float deltaSeconds, ActiveNpc& npc, const Visual& visual);
        // Advances a patrolling NPC: rests at a waypoint, then occasionally walks
        // to the next, updating position/yaw. Sets npc.patrolMoving, which drives
        // the walk-vs-idle choice in update_animation. No-op for static NPCs.
        void update_patrol(float deltaSeconds, ActiveNpc& npc, const Visual& visual);
        // Removes map NPCs beyond a despawn radius (and the farthest over a hard
        // budget), resetting their placements so they re-stream when near again.
        void despawn_distant(const phoenix::renderer::CameraView& view);
        // Under texture-slot pressure, evicts the least-recently-used visuals that
        // no active NPC references, freeing their texture slots for reuse.
        void evict_visuals_if_needed(phoenix::renderer::VulkanRenderer& renderer);
        // Frees one cached visual: returns its texture slots (refcounted) to the
        // free list and drops it from visuals_/modelIndexOffset_. Caller rebuilds.
        void evict_visual(std::uint32_t modelIndex);

        // One unique pose to skin this frame. Built serially (a snapshot of the
        // entity's animation state), then written in parallel — the result goes
        // to paletteFloats_[baseBone*16 ..]. Entities sharing a pose share a job.
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
        // Thread-safe for disjoint baseBone regions (uses thread-local scratch),
        // so jobs can be skinned in parallel across the worker pool.
        void write_palette_at(const PaletteJob& job);

        // A map NPC entry awaiting (or after) lazy streaming. Coordinates are
        // already in engine/world space; waypoint y is the authored ground Y (no
        // terrain clamp). A single waypoint is a static NPC; multiple waypoints
        // make it a patrol. `spawned` flips once turned into an ActiveNpc —
        // placements are never un-streamed within a map. The anchor (x,z) used
        // for the streaming range test is the first waypoint.
        struct MapPlacement
        {
            std::size_t catalogIndex{};
            std::uint32_t modelIndex{};
            std::vector<Waypoint> waypoints;   // engine space, raw authored Y
            bool spawned{};
        };

        std::vector<NpcCatalogEntry> catalog_;
        // (npcType << 32 | npcTypeId) -> catalogIndex, for resolving svmap NPCs.
        std::unordered_map<std::uint64_t, std::size_t> catalogByTypeKey_;
        std::unordered_map<std::uint32_t, std::vector<VisualPartRow>> visualRows_;
        std::unordered_map<std::uint32_t, Visual> visuals_;
        // In-flight async visual loads (modelIndex -> future). A model is loading
        // while present here; once finalized it moves to visuals_.
        std::unordered_map<std::uint32_t, std::future<std::shared_ptr<Visual>>> visualLoads_;
        // Models whose async load failed — placements of these are dropped rather
        // than retried every frame.
        std::unordered_set<std::uint32_t> failedModels_;
        std::unordered_map<std::string, std::uint32_t> textureSlotByPath_;
        // Texture-slot lifetime: a slot is shared by all visuals using the same
        // texture; freed (returned to freeTextureSlots_) when its last user is
        // evicted. textureKeyBySlot_ is the inverse of textureSlotByPath_.
        std::unordered_map<std::uint32_t, std::uint32_t> slotRefcount_;
        std::unordered_map<std::uint32_t, std::string> textureKeyBySlot_;
        std::vector<std::uint32_t> freeTextureSlots_;
        // modelIndex -> frameCounter_ when it last had a visible NPC (LRU order).
        std::unordered_map<std::uint32_t, std::uint32_t> modelLastUsedFrame_;
        std::filesystem::path catalogRoot_;
        std::vector<ActiveNpc> active_;
        std::vector<NpcLabel> labels_;   // rebuilt each update() for visible NPCs
        std::vector<phoenix::renderer::SkinnedVertex> renderVertices_;
        std::vector<std::uint32_t> renderIndices_;
        // One shared bind mesh per model: modelIndex -> its first index in
        // renderIndices_. All entities of a model draw this geometry, instanced.
        std::unordered_map<std::uint32_t, std::uint32_t> modelIndexOffset_;
        std::vector<float> paletteFloats_;   // per-frame concatenated bone palettes
        std::vector<phoenix::renderer::ObjectInstance> instances_;
        std::vector<phoenix::renderer::ObjectBatch> instanceBatches_;
        // Map (.svmap) NPC streaming state. mapDataRoot_/mapTextureBaseSlot_/
        // mapTextureReserve_ are captured by load_map_npcs so stream_map_npcs can
        // load visuals on demand from inside update().
        std::vector<MapPlacement> placements_;
        std::filesystem::path mapDataRoot_;
        std::uint32_t mapTextureBaseSlot_{};
        std::uint32_t mapTextureReserve_{};
        std::size_t streamedPlacements_{};
        // Drives randomized patrol rest intervals so patrols don't move in sync.
        std::mt19937 patrolRng_{ 0x9E3779B9u };
        std::uint32_t nextTextureSlot_{};
        std::uint32_t frameCounter_{};   // frame stamp for visual-eviction LRU
        std::size_t visibleCount_{};
        std::size_t lastStatusVisible_{ static_cast<std::size_t>(-1) };
        std::string status_;
    };
}
