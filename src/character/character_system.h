#pragma once

#include "renderer/vulkan_renderer.h"
#include "world/character_loader.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::character
{
    struct CharacterAppearance
    {
        std::string raceFolder{ "Human" };
        std::string prefix{ "huwm" };
        int armorIndex{ 4 };
        int faceIndex{ 1 };
        int hairIndex{ 1 };
    };

    // Character GPU vertices match TerrainVertex layout so they can render
    // through the existing terrain pipeline without a separate shader/pipeline.
    struct CharacterGpuVertex
    {
        float position[3]{};
        float color[3]{ 1.0f, 1.0f, 1.0f };
        float normal[3]{};
        float uv[2]{};
        std::uint32_t textureLayer{ 0xFFFFFFFFu };
    };
    static_assert(sizeof(CharacterGpuVertex) == sizeof(phoenix::renderer::TerrainVertex),
        "CharacterGpuVertex must match TerrainVertex layout");

    struct CharacterBatch
    {
        std::uint32_t textureIndex{};
        std::uint32_t startIndex{};
        std::uint32_t indexCount{};
        bool alphaCutout{};
    };

    struct CharacterAnimationChoice
    {
        std::filesystem::path path;
        std::string name;
        world::CharacterAnimation animation;
    };

    // Input for the playable character mode.
    struct PlayableInput
    {
        bool forward{};
        bool backward{};
        bool left{};
        bool right{};
        bool jump{};
        bool fast{};
        bool yawLeft{};
        bool yawRight{};
        bool pitchUp{};
        bool pitchDown{};
        bool cameraDrag{};
        float mouseDx{};
        float mouseDy{};
        float mouseWheel{};
    };

    // Holds all data for a loaded character (mesh, bones, animations).
    struct CharacterData
    {
        struct SourceVertex
        {
            float position[3]{};
            float normal[3]{};
            float uv[2]{};
            float weights[3]{};
            std::uint8_t bones[3]{};
            std::uint32_t meshBoneBase{};
            std::uint32_t meshBoneCount{};
        };

        std::vector<CharacterGpuVertex> bindVertices;
        std::vector<SourceVertex> sourceVertices;
        std::vector<world::CharacterBone> meshBones;
        std::vector<std::uint32_t> indices;
        std::vector<CharacterBatch> batches;
        std::vector<std::filesystem::path> texturePaths;
        std::vector<CharacterAnimationChoice> animations;

        // Named animation slots.
        std::size_t idleAnimation{};
        std::size_t walkAnimation{};
        std::size_t runAnimation{};
        std::size_t backAnimation{};
        std::size_t leftAnimation{};
        std::size_t rightAnimation{};
        std::size_t jumpAnimation{};
        std::size_t swimIdleAnimation{};
        std::size_t swimAnimation{};

        bool loaded{};
    };

    // Height sampling callback — main.cpp provides terrain height at world XZ.
    using HeightSampleFn = float(*)(float worldX, float worldZ, void* userData);

    // Collision callback — given proposed position, writes back adjusted position.
    // Returns true if a collision occurred and position was adjusted.
    using CollisionFn = bool(*)(float proposedX, float proposedZ,
        float previousX, float previousZ,
        float characterY,
        float& outX, float& outZ, void* userData);

    class CharacterSystem
    {
    public:
        // Load a character from the data root.
        bool preload(const std::filesystem::path& dataRoot);
        bool load(const std::filesystem::path& dataRoot);
        bool load(const std::filesystem::path& dataRoot, const CharacterAppearance& appearance);

        // Set the base texture layer index into the shared texture array.
        void set_texture_layer_base(std::uint32_t base) { textureLayerBase_ = base; }

        // Set height sampling function (for terrain following).
        void set_height_sampler(HeightSampleFn fn, void* userData) { heightFn_ = fn; heightUserData_ = userData; }

        // Set collision callback (for world object collision).
        void set_collision_callback(CollisionFn fn, void* userData) { collisionFn_ = fn; collisionUserData_ = userData; }

        // Update playable mode: movement, physics, animation, vertex skinning.
        void update(float deltaSeconds, const PlayableInput& input);

        // Get the camera position/orientation for the third-person view.
        void camera_state(float& x, float& y, float& z, float& yaw, float& pitch) const;
        float camera_distance() const { return cameraDistance_; }

        // Access GPU data for upload.
        const std::vector<CharacterGpuVertex>& world_vertices() const { return worldVertices_; }
        const std::vector<std::uint32_t>& indices() const { return data_.indices; }
        const std::vector<CharacterBatch>& batches() const { return data_.batches; }
        const std::vector<std::filesystem::path>& texture_paths() const { return data_.texturePaths; }
        const std::vector<std::filesystem::path>& cached_texture_paths() const { return cachedTexturePaths_; }
        bool ready() const { return data_.loaded; }

        // Character world position.
        float world_x() const { return characterX_; }
        float world_y() const { return characterY_; }
        float world_z() const { return characterZ_; }
        void set_world_position(float x, float y, float z, float yaw = 0.0f);

    private:
        void skin_and_transform();

        CharacterData data_;
        std::vector<CharacterGpuVertex> worldVertices_;
        std::uint32_t textureLayerBase_{};
        bool cacheReady_{};
        std::filesystem::path cachedDataRoot_;
        std::unordered_map<std::string, world::CharacterModel> cachedModels_;
        std::unordered_map<std::string, std::vector<CharacterAnimationChoice>> cachedAnimations_;
        std::unordered_map<std::string, std::uint32_t> cachedTextureSlotByPath_;
        std::vector<std::filesystem::path> cachedTexturePaths_;

        // Playable state.
        float characterX_{};
        float characterY_{};
        float characterZ_{};
        float characterYaw_{};
        float verticalVelocity_{};
        float cameraYaw_{};
        float cameraPitch_{ -0.16f };
        float cameraDistance_{ 6.2f };
        float animationSeconds_{};
        std::size_t activeAnimation_{};
        bool grounded_{ true };
        bool jumpWasDown_{};
        bool groundInitialized_{};

        HeightSampleFn heightFn_{};
        void* heightUserData_{};
        CollisionFn collisionFn_{};
        void* collisionUserData_{};
        bool inWater_{};
    };
}
