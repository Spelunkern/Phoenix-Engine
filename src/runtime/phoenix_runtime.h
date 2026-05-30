#pragma once

#include "assets/data_index.h"
#include "renderer/vulkan_renderer.h"
#include "world/actor_scene.h"
#include "world/eft_loader.h"
#include "world/mani_loader.h"
#include "world/wld_loader.h"

#include <filesystem>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>


namespace phoenix::runtime
{
    struct EntityAsset
    {
        std::filesystem::path path;
        std::string displayName;
        std::string section; // WLD section: Building, Shape, Tree, Grass, etc.
    };

    struct AudioAsset
    {
        std::filesystem::path path;
        std::string displayName;
        std::string fileName;
    };

    struct LoadedWorldAsset
    {
        std::string name;
        std::filesystem::path path;
        float radius{ 16.0f };
        std::uint32_t vertices{};
        bool loaded{};
        bool vertexAnimated{};
        std::uint32_t frameCount{ 1 };
        std::vector<phoenix::renderer::TerrainVertex> previewVertices;
        std::vector<std::uint32_t> previewIndices;
        std::vector<std::vector<phoenix::renderer::TerrainVertex>> animationFrames;
        // Collision mesh in model-local space.
        bool hasCollision{};
        std::vector<float> collisionVertices; // flat x,y,z per vertex
        std::vector<std::uint32_t> collisionIndices; // 3 per triangle
    };

    struct SceneObject
    {
        float x{};
        float y{};
        float z{};
        float forward[3]{};
        float up[3]{};
        float radius{ 18.0f };
        std::int32_t assetSlot{ -1 };
        bool loaded{};
        std::int32_t sectionIndex{ -1 };
        std::int32_t instanceIndex{ -1 };
        bool deleted{};
        bool animated{};
        std::int32_t maniAssetIndex{ -1 };
    };

    struct WaterAnimation
    {
        float tileSize{ 64.0f };
        std::uint32_t frameCount{};
        std::vector<std::string> frameFileNames; // e.g. "caust00.dds"
        std::vector<std::filesystem::path> framePaths;
    };

    struct PhoenixRuntimeState
    {
        std::filesystem::path dataRoot;
        std::filesystem::path entityRoot;
        phoenix::assets::DataIndex assets;
        phoenix::world::WldAnalysis world;
        std::vector<EntityAsset> entityAssets;
        std::vector<LoadedWorldAsset> worldAssets;
        std::vector<phoenix::world::ManiAnimation> maniAnimations;
        std::vector<std::filesystem::path> assetTexturePaths;
        std::unordered_map<std::string, std::uint32_t> textureSlotByPath;
        std::vector<SceneObject> sceneObjects;
        std::vector<std::filesystem::path> worldMapPaths;
        std::vector<std::string> worldMapNames;
        std::vector<std::string> skyFileNames;
        std::vector<std::string> terrainTextureNames;
        std::vector<AudioAsset> audioAssets;
        WaterAnimation waterAnimation;
        phoenix::world::EftFile eftFile;
        std::vector<std::filesystem::path> effectTexturePaths;
        std::vector<std::int32_t> effectInstanceTextureSlot; // per effect instance: texture layer or -1
        std::size_t selectedWorldMap{};
        std::string status;
    };

    struct CameraInput
    {
        bool forward{};
        bool backward{};
        bool left{};
        bool right{};
        bool up{};
        bool down{};
        bool fast{};
        bool look{};
        bool yawLeft{};
        bool yawRight{};
        bool pitchUp{};
        bool pitchDown{};
        float mouseDx{};
        float mouseDy{};
        float wheel{};
    };

    struct RuntimeCamera
    {
        float x{};
        float y{ 360.0f };
        float z{ -950.0f };
        float yaw{};
        float pitch{ -0.32f };
        float speed{ 320.0f };
    };

    // Live-tunable mob/NPC animation parameters (exposed in the debug UI so the
    // playback can be matched to the native client). Defaults are the calibrated
    // values; changing them takes effect immediately.
    struct ActorAnimTuning
    {
        float breathFps{ 12.0f };          // idle "breathing" loop rate
        float walkFps{ 14.0f };            // mob walk loop rate
        float runFps{ 18.0f };             // mob run loop rate
        float npcBreathFps{ 12.0f };       // NPC idle loop rate
        float decorFps{ 12.0f };           // non-actor decorative vertex anims
        float mobWalkSpeed{ 1.8f };        // world units/s while walking
        float mobRunSpeed{ 4.5f };         // world units/s while running
        float mobRoamRadius{ 12.0f };      // wander radius around home
        float mobIdleMin{ 4.0f };          // min idle seconds between roams
        float mobIdleMax{ 12.0f };         // max idle seconds between roams
        float gestureIntervalMin{ 20.0f }; // seconds between idle gestures
        float gestureIntervalMax{ 30.0f };
        float idleGestureDuration{ 5.0f }; // how long an idle gesture plays
        float animationRange{ 180.0f };    // distance beyond which actors freeze
        float moveAnimThreshold{ 0.4f };   // fraction of a type moving -> walk/run anim
    };

    struct PreviewImage
    {
        std::uint32_t width{};
        std::uint32_t height{};
        std::vector<std::uint8_t> bgra;
    };

    struct StaticObjectScene
    {
        struct BatchBounds
        {
            float x{};
            float y{};
            float z{};
            float radius{};
        };

        std::vector<phoenix::renderer::TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<phoenix::renderer::ObjectInstance> instances;
        std::vector<phoenix::renderer::ObjectBatch> batches;
        std::vector<BatchBounds> batchBounds;
    };

    struct AnimatedObjectScene
    {
        struct VertexAnimation
        {
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
            std::vector<std::vector<phoenix::renderer::TerrainVertex>> frames; // world VANI only
            phoenix::world::ActorSkinData skinData;
            phoenix::world::ActorAnimationSet animations;
            float worldX{};
            float worldY{};
            float worldZ{};
            float boundingRadius{ 48.0f };
            float gestureTimer{};
            bool playingGesture{};
            bool isMob{};
            bool hasActorSkin{};
            std::int32_t cachedFrame{ -1 };
            const phoenix::world::CharacterAnimation* cachedAnim{};
            // Mob type-level movement state.
            std::uint32_t totalInstances{};
            std::uint32_t movingCount{};
            std::uint32_t runningCount{};
            float mobMoveTimer{};
            bool mobMoving{};
            bool mobRunning{};
        };

        struct InstanceAnimation
        {
            std::uint32_t instanceIndex{};
            float axis[3]{};
            float speed{};
        };

        // Per-instance movement state for mobs.
        struct MobInstanceState
        {
            std::uint32_t instanceIndex{};
            std::uint32_t animIndex{};   // vertex animation index this instance belongs to
            float spawnX{};
            float spawnZ{};
            float targetX{};
            float targetZ{};
            float moveTimer{};
            float yaw{};
            bool moving{};
            bool running{};
        };

        std::vector<phoenix::renderer::TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<phoenix::renderer::ObjectInstance> baseInstances;
        std::vector<phoenix::renderer::ObjectInstance> instances;
        std::vector<phoenix::renderer::ObjectBatch> batches;
        std::vector<StaticObjectScene::BatchBounds> batchBounds;
        std::vector<VertexAnimation> vertexAnimations;
        std::vector<InstanceAnimation> instanceAnimations;
        std::vector<MobInstanceState> mobInstances;
    };

    // World-space collision triangles with spatial grid for fast queries.
    // World-space collision triangles with spatial grid for fast queries.
    struct WorldCollisionMesh
    {
        struct Triangle
        {
            float v0[3]{};
            float v1[3]{};
            float v2[3]{};
            float minY{};
            float maxY{};
            float normalY{}; // Y component of face normal (>0 = floor-like, ~0 = wall)
        };

        std::vector<Triangle> triangles;

        // Spatial grid for fast lookup.
        static constexpr float kCellSize = 8.0f;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> grid;

        // Threshold: triangles with normalY above this are walkable floors, not walls.
        static constexpr float kWalkableNormalY = 0.55f; // ~56 degree slope

        static std::uint64_t cell_key(int cx, int cz)
        {
            return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(cx)) << 32)
                | static_cast<std::uint64_t>(static_cast<std::uint32_t>(cz));
        }

        void build_grid();
        bool check_collision(float prevX, float prevZ, float& proposedX, float& proposedZ,
            float characterY, float characterHeight, float characterRadius) const;

        // Query floor height from walkable collision surfaces at (worldX, worldZ).
        // Returns the highest walkable surface below or at characterY + stepHeight.
        // If no walkable surface is found, returns belowY (pass a very low default).
        float floor_height_at(float worldX, float worldZ, float characterY, float stepHeight) const;
    };

    class PhoenixRuntime
    {
    public:
        bool initialize(const std::filesystem::path& executableDir, bool loadDefaultMap = true);
        bool load_world_map(std::size_t mapIndex);
        PreviewImage create_preview_image(std::uint32_t width, std::uint32_t height) const;
        PreviewImage create_3d_preview_image(std::uint32_t width, std::uint32_t height) const;
        void build_terrain_mesh(std::vector<phoenix::renderer::TerrainVertex>& vertices, std::vector<std::uint32_t>& indices) const;
        void build_debug_gizmo_mesh(
            bool includeSounds,
            bool includeMusic,
            bool includePortals,
            bool includeEffects,
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            std::vector<std::uint32_t>& indices) const;
        StaticObjectScene build_static_object_scene() const;
        AnimatedObjectScene build_animated_object_scene() const;
        void update_animated_object_scene(AnimatedObjectScene& scene, float totalTime, float deltaSeconds,
            float cameraX, float cameraY, float cameraZ,
            std::size_t actorVertexAnimationStart = std::numeric_limits<std::size_t>::max(),
            bool skipActorSkinning = false) const;
        std::vector<std::filesystem::path> terrain_texture_paths() const;
        std::vector<std::filesystem::path> asset_texture_paths() const;
        std::filesystem::path texture_path_for(std::string_view fileName) const;
        std::filesystem::path audio_path_for(std::string_view fileName) const;
        std::filesystem::path water_texture_path() const;
        bool load_water_animation();
        const WaterAnimation& water_animation() const { return state_.waterAnimation; }
        bool load_effect_data();
        std::vector<std::filesystem::path> effect_texture_paths() const;
        void set_effect_texture_base(std::uint32_t base) { effectTextureBase_ = base; }
        void build_effect_billboards(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            float totalTime = 0.0f) const;
        std::filesystem::path sky_texture_path() const;
        void camera_state(float& x, float& y, float& z, float& yaw, float& pitch) const;
        void update_camera(float deltaSeconds, const CameraInput& input);
        std::string window_title(const std::string& rendererName, float fps, bool fogEnabled) const;
        const std::vector<std::string>& world_map_names() const { return state_.worldMapNames; }
        const std::vector<std::string>& sky_file_names() const { return state_.skyFileNames; }
        const std::vector<std::string>& terrain_texture_names() const { return state_.terrainTextureNames; }
        const std::vector<AudioAsset>& audio_assets() const { return state_.audioAssets; }
        std::size_t selected_world_map() const { return state_.selectedWorldMap; }
        const PhoenixRuntimeState& state() const { return state_; }
        ActorAnimTuning& actor_anim_tuning() { return actorAnimTuning_; }

        float terrain_height_at(float worldX, float worldZ) const { return terrain_height(worldX, worldZ); }
        WorldCollisionMesh build_collision_mesh() const;

    private:
        std::filesystem::path find_data_root(const std::filesystem::path& executableDir) const;
        void scan_entity_assets();
        void scan_world_maps();
        void scan_sky_assets();
        void scan_terrain_textures();
        void scan_audio_assets();
        void load_world_assets();
        std::uint32_t resolve_asset_texture_layer(std::string_view textureName, bool forceCutout);
        void update_status();
        float terrain_height(float worldX, float worldZ) const;

        PhoenixRuntimeState state_;
        RuntimeCamera camera_;
        ActorAnimTuning actorAnimTuning_;
        std::uint32_t effectTextureBase_{};
        // Memoises resolve_asset_texture_layer by (textureName, forceCutout) so the
        // per-mesh world build doesn't re-resolve+stat the same texture thousands of
        // times. Cleared at the start of each load_world_assets().
        std::unordered_map<std::string, std::uint32_t> assetTextureLayerCache_;
    };
}
