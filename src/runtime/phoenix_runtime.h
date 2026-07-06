#pragma once

#include "assets/data_index.h"
#include "renderer/opengl_renderer.h"
#include "world/wld_loader.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <memory>
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
        // Shared with AnimatedObjectScene::VertexAnimation — VANI frame data is
        // large (frames x vertices), so scenes reference it instead of copying.
        std::shared_ptr<std::vector<std::vector<phoenix::renderer::TerrainVertex>>> animationFrames;
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
        float maniRotationAxis[3]{};
        float maniRotationSpeed{};
        bool deleted{};
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
        std::vector<std::filesystem::path> assetTexturePaths;
        std::unordered_map<std::string, std::uint32_t> textureSlotByPath;
        std::vector<SceneObject> sceneObjects;
        std::vector<std::filesystem::path> worldMapPaths;
        std::vector<std::string> worldMapNames;
        std::vector<std::string> skyFileNames;
        std::vector<std::string> terrainTextureNames;
        std::vector<AudioAsset> audioAssets;
        WaterAnimation waterAnimation;
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
            std::uint32_t firstIndex{};
            std::uint32_t indexCount{};
            std::uint32_t firstInstance{};
            std::uint32_t instanceCount{};
            std::uint32_t currentFrame{ 0xFFFFFFFFu };
            bool visible{ true };
            std::shared_ptr<const std::vector<std::vector<phoenix::renderer::TerrainVertex>>> frames;
        };

        std::vector<phoenix::renderer::TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<phoenix::renderer::ObjectInstance> baseInstances;
        std::vector<phoenix::renderer::ObjectInstance> instances;
        std::vector<phoenix::renderer::ObjectBatch> batches;
        std::vector<StaticObjectScene::BatchBounds> batchBounds;
        std::vector<VertexAnimation> vertexAnimations;

        struct DirtyVertexRange
        {
            std::uint32_t firstVertex{};
            std::uint32_t vertexCount{};
        };

        // Dirty vertex range tracking: upload each animated asset range separately.
        std::vector<DirtyVertexRange> dirtyVertexRanges;
        void mark_vertices_dirty(std::uint32_t first, std::uint32_t count)
        {
            dirtyVertexRanges.push_back({ first, count });
        }
        void clear_dirty() { dirtyVertexRanges.clear(); }

        // Coalesce overlapping/contiguous ranges so adjacent assets upload as
        // one memcpy instead of several small ones.
        void merge_dirty_ranges()
        {
            if (dirtyVertexRanges.size() < 2)
                return;
            std::sort(dirtyVertexRanges.begin(), dirtyVertexRanges.end(),
                [](const DirtyVertexRange& a, const DirtyVertexRange& b) {
                    return a.firstVertex < b.firstVertex;
                });
            std::size_t mergedCount = 0;
            for (std::size_t i = 1; i < dirtyVertexRanges.size(); ++i)
            {
                auto& merged = dirtyVertexRanges[mergedCount];
                const auto& next = dirtyVertexRanges[i];
                if (next.firstVertex <= merged.firstVertex + merged.vertexCount)
                {
                    const auto end = std::max(merged.firstVertex + merged.vertexCount,
                        next.firstVertex + next.vertexCount);
                    merged.vertexCount = end - merged.firstVertex;
                }
                else
                {
                    dirtyVertexRanges[++mergedCount] = next;
                }
            }
            dirtyVertexRanges.resize(mergedCount + 1);
        }
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

        // True if collision geometry blocks the segment a->b strictly between its
        // endpoints. Used to occlude in-world NPC labels behind buildings/assets.
        bool segment_occluded(const float a[3], const float b[3]) const;
    };

    class PhoenixRuntime
    {
    public:
        bool initialize(const std::filesystem::path& executableDir, bool loadDefaultMap = true);
        bool load_world_map(std::size_t mapIndex);
        PreviewImage create_3d_preview_image(std::uint32_t width, std::uint32_t height) const;
        // LOD info for terrain chunks — returned alongside the mesh so the
        // visibility builder can pick the right index range per distance.
        static constexpr int kTerrainLodLevels = 4;   // stride 1, 2, 4, 8
        static constexpr int kTerrainChunkQuads = 16;
        struct TerrainChunkLod
        {
            std::uint32_t firstIndex{};
            std::uint32_t indexCount{};
        };
        struct TerrainLodInfo
        {
            std::uint32_t chunkCountX{};
            std::uint32_t chunkCountZ{};
            std::uint32_t grid{};
            float cellSize{};
            float halfMap{};
            // [chunkZ * chunkCountX + chunkX][lod]
            std::vector<std::array<TerrainChunkLod, kTerrainLodLevels>> chunks;
        };
        void build_terrain_mesh(std::vector<phoenix::renderer::TerrainVertex>& vertices, std::vector<std::uint32_t>& indices, TerrainLodInfo& lodInfo) const;
        StaticObjectScene build_static_object_scene() const;
        AnimatedObjectScene build_animated_object_scene() const;
        void update_animated_object_scene(AnimatedObjectScene& scene, float totalTime,
            float cameraX, float cameraY, float cameraZ) const;
        std::vector<std::filesystem::path> terrain_texture_paths() const;
        std::vector<std::filesystem::path> asset_texture_paths() const;

        // Field lightmap/alpha layer paths (Data/World/field/<mapId>/).
        // Returns up to 4 lightmap paths (one per section, 2x2 for big maps, 1x1 for small).
        // sectionCount is set to 1 (small) or 2 (big = 2x2 grid).
        std::vector<std::filesystem::path> field_lightmap_paths(std::uint32_t& sectionCount) const;

        // Field alpha splat masks (<stem>_<sec>_a<n>.dds): per-layer terrain
        // blend weights ("tonality" maps). Returns sections x 8 paths (empty
        // where missing) and a bitmask of layers that have at least one mask.
        static constexpr std::uint32_t kFieldAlphaMaskLayers = 8;
        std::vector<std::filesystem::path> field_alpha_mask_paths(std::uint32_t& layerFlags) const;

        // Dungeon lightmap pages (<dgName>_L<i>.dds next to the DG model),
        // carrying both baked shadows and colour tones.
        std::vector<std::filesystem::path> dungeon_lightmap_paths() const;

        // Climbable volumes from the WLD "Object" section (entity/object/
        // ladders & ivy): world-space axis + vertical range per instance.
        struct LadderVolume
        {
            float x{}, z{};
            float baseY{};
            float topY{};
            float radius{};
        };
        std::vector<LadderVolume> ladder_volumes() const;
        std::filesystem::path texture_path_for(std::string_view fileName) const;
        std::filesystem::path audio_path_for(std::string_view fileName) const;
        std::filesystem::path water_texture_path() const;
        bool load_water_animation();
        const WaterAnimation& water_animation() const { return state_.waterAnimation; }
        std::filesystem::path sky_texture_path() const;
        std::filesystem::path walk_sound_at(float worldX, float worldZ) const;
        void camera_state(float& x, float& y, float& z, float& yaw, float& pitch) const;
        void set_camera_position(float x, float y, float z, float yaw, float pitch);
        void update_camera(float deltaSeconds, const CameraInput& input);
        std::string window_title(const std::string& rendererName, float fps, bool fogEnabled) const;
        const std::vector<std::string>& world_map_names() const { return state_.worldMapNames; }
        const std::vector<std::string>& sky_file_names() const { return state_.skyFileNames; }
        const std::vector<std::string>& terrain_texture_names() const { return state_.terrainTextureNames; }
        const std::vector<AudioAsset>& audio_assets() const { return state_.audioAssets; }
        std::size_t selected_world_map() const { return state_.selectedWorldMap; }
        const PhoenixRuntimeState& state() const { return state_; }
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
        std::uint32_t resolve_asset_texture_layer(std::string_view textureName);
        void update_status();
        float terrain_height(float worldX, float worldZ) const;

        PhoenixRuntimeState state_;
        RuntimeCamera camera_{};
        // Memoises resolve_asset_texture_layer by textureName so the per-mesh
        // world build doesn't re-resolve+stat the same texture thousands of
        // times. Cleared at the start of each load_world_assets().
        std::unordered_map<std::string, std::uint32_t> assetTextureLayerCache_;
        // Universal transparency cache: lowercase texture path -> "has cutout
        // alpha" (decided by alpha content, never by name). Persists across
        // map loads — texture content doesn't change at runtime.
        std::unordered_map<std::string, bool> textureCutoutCache_;
    };
}
