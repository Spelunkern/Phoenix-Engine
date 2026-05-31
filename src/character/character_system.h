#pragma once

#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"
#include "world/character_loader.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace phoenix::character
{
    // Weapon type IDs matching the Item CSV folder structure.
    enum class WeaponType : int
    {
        None        = 0,
        Sword1H     = 1,   // One-hand swords
        Sword2H     = 2,   // Two-handed swords
        Axe1H       = 3,   // One-hand axes
        Axe2H       = 4,   // Two-handed axes
        DualSword   = 5,   // Dual swords and axes
        Spear       = 6,   // Spears
        Mace1H      = 7,   // One-hand maces
        Hammer2H    = 8,   // Two-hand hammers
        RevDagger   = 9,   // Reverse daggers
        Dagger      = 10,  // Daggers
        Javelin     = 11,  // Javelins
        Staff       = 12,  // Staves (magical)
        Bow         = 13,  // Bows
        Crossbow    = 14,  // Crossbows
        Claw        = 15,  // Claws / knuckles
        ShieldLight = 19,  // Shields (light faction)
        ShieldDark  = 34,  // Shields (dark faction)
    };

    struct CharacterAppearance
    {
        std::string raceFolder{ "Human" };
        std::string prefix{ "humf" };
        int upperIndex{ 19 };
        int lowerIndex{ 19 };
        int handIndex{ 19 };
        int footIndex{ 19 };
        int helmetIndex{ 9 };
        int faceIndex{ 1 };
        int hairIndex{ 1 };
        bool helmetVisible{ true };
        // Weapon / shield. Default loadout: one-hand sword + light shield.
        WeaponType weaponType{ WeaponType::Sword1H };
        int weaponIndex{ 1 };    // RecordIndex in the weapon type CSV (-1 = none)
        WeaponType shieldType{ WeaponType::ShieldLight };
        int shieldIndex{ 1 };    // RecordIndex in the shield type CSV (-1 = none)
        // Cloak. Default to cloak design 1.
        int cloakIndex{ 1 };     // Index into CTL texture list (-1 = none)
        // Mount (vehicle). Vehicles are monsters game-mechanically; the rider
        // plays its mounted animations seated on a bone of the mount.
        bool mounted{ false };
        std::string mountClass{ "hu" };  // hu / de / el / vi (vehicle CSV class)
        int mountIndex{ 0 };             // RecordIndex into vehicle_{class}_01.csv
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
            std::uint32_t gpuIndex{};         // target slot in bindVertices/worldVertices
        };

        std::vector<CharacterGpuVertex> bindVertices;
        std::vector<SourceVertex> sourceVertices;
        std::vector<world::CharacterBone> meshBones;
        std::vector<std::uint32_t> indices;
        std::vector<CharacterBatch> batches;
        std::vector<std::filesystem::path> texturePaths;
        std::vector<CharacterAnimationChoice> animations;

        // Weapon/shield — static meshes attached to hand bones.
        // Stored as raw model-space vertices; transformed by hand bone during skinning.
        struct WeaponPart
        {
            std::vector<world::ItemVertex> vertices;
            std::vector<world::CharacterFace> faces;
            std::uint32_t textureIndex{};     // index into texturePaths
            bool alphaCutout{};
            std::uint32_t vertexOffset{};     // offset into bindVertices where this part starts
            std::uint32_t indexOffset{};      // offset into indices where this part starts
            std::uint32_t vertexCount{};
            std::uint32_t indexCount{};
        };
        WeaponPart weapon;
        WeaponPart shield;
        bool hasWeapon{};
        bool hasShield{};

        // Cloak — static meshes (3DC with boneCount=0), bone-attached.
        // Two components: main cloak body + shoulder piece.
        WeaponPart cloakBody;
        WeaponPart cloakShoulder;
        bool hasCloak{};

        // Mount (vehicle) — a full skinned monster model with its own skeleton
        // and animation set, rendered together with the rider. The rider is
        // seated on one of the mount's bones (default bone 1). The mount's GPU
        // vertices live in bindVertices/indices/batches (shared render path),
        // but its source vertices + bones are kept separate so the rider's
        // single-animation skin loop never touches them.
        struct MountData
        {
            std::vector<SourceVertex> sourceVertices;     // mount skinned verts
            std::vector<world::CharacterBone> meshBones;  // mount mesh bind bones
            std::vector<CharacterAnimationChoice> animations; // idle/walk/run/jump/br
            std::size_t idleAnimation{};
            std::size_t walkAnimation{};
            std::size_t runAnimation{};
            std::size_t jumpAnimation{};
            std::size_t breathAnimation{};
            std::uint32_t vertexOffset{};  // first mount vertex in bindVertices
            std::uint32_t vertexCount{};
            float scale{ 1.0f };
            bool loaded{};
        };
        MountData mount;
        bool hasMount{};

        // Animation slots resolved by action CSV record index.
        // Movement
        std::size_t idleAnimation{};
        std::size_t walkAnimation{};
        std::size_t runAnimation{};
        std::size_t backAnimation{};
        std::size_t leftAnimation{};
        std::size_t rightAnimation{};
        std::size_t swimIdleAnimation{};
        std::size_t swimAnimation{};
        std::size_t jumpAnimation{};
        std::size_t dieAnimation{};
        std::size_t sitDownAnimation{};
        std::size_t sitUpAnimation{};
        std::size_t sitAnimation{};
        std::size_t dodgeBackAnimation{};
        std::size_t dodgeLeftAnimation{};
        std::size_t dodgeRightAnimation{};
        std::size_t idle1Animation{};
        std::size_t idle2Animation{};
        std::size_t ladderAnimation{};
        std::size_t selectAnimation{};
        // Mounted
        std::size_t vehicleRun1Animation{};
        std::size_t vehicleIdleAnimation{};
        std::size_t vehicleRun2Animation{};
        // Two-hand weapons
        std::size_t twoHandReadyAnimation{};
        std::size_t twoHandAttack1Animation{};
        std::size_t twoHandAttack2Animation{};
        std::size_t twoHandAttack3Animation{};
        std::size_t twoHandAttack4Animation{};
        std::size_t twoHandDamageAnimation{};
        std::size_t twoHandRunAnimation{};
        // Bows
        std::size_t bowReadyAnimation{};
        std::size_t bowAttackAnimation{};
        std::size_t bowDamageAnimation{};
        std::size_t bowRunAnimation{};
        // One-hand weapons
        std::size_t oneHandReadyAnimation{};
        std::size_t oneHandAttack1Animation{};
        std::size_t oneHandAttack2Animation{};
        std::size_t oneHandAttack3Animation{};
        std::size_t oneHandAttack4Animation{};
        std::size_t oneHandDamageAnimation{};
        std::size_t oneHandRunAnimation{};
        // Dual weapons
        std::size_t dualReadyAnimation{};
        std::size_t dualAttack1Animation{};
        std::size_t dualAttack2Animation{};
        std::size_t dualAttack3Animation{};
        std::size_t dualAttack4Animation{};
        std::size_t dualDamageAnimation{};
        std::size_t dualRunAnimation{};
        // Spears
        std::size_t spearReadyAnimation{};
        std::size_t spearAttack1Animation{};
        std::size_t spearAttack2Animation{};
        std::size_t spearAttack3Animation{};
        std::size_t spearAttack4Animation{};
        std::size_t spearDamageAnimation{};
        std::size_t spearRunAnimation{};
        // Crossbow
        std::size_t crossbowReadyAnimation{};
        std::size_t crossbowAttackAnimation{};
        std::size_t crossbowDamageAnimation{};
        std::size_t crossbowRunAnimation{};
        // Staff
        std::size_t staffReadyAnimation{};
        std::size_t staffAttack1Animation{};
        std::size_t staffAttack2Animation{};
        std::size_t staffDamageAnimation{};
        std::size_t staffRunAnimation{};
        // Reverse dagger
        std::size_t revDaggerReadyAnimation{};
        std::size_t revDaggerAttack1Animation{};
        std::size_t revDaggerAttack2Animation{};
        std::size_t revDaggerAttack3Animation{};
        std::size_t revDaggerAttack4Animation{};
        std::size_t revDaggerDamageAnimation{};
        std::size_t revDaggerRunAnimation{};
        // Knuckles / claws
        std::size_t knuckleReadyAnimation{};
        std::size_t knuckleAttack1Animation{};
        std::size_t knuckleAttack2Animation{};
        std::size_t knuckleAttack3Animation{};
        std::size_t knuckleAttack4Animation{};
        std::size_t knuckleDamageAnimation{};
        std::size_t knuckleRunAnimation{};
        // Dagger
        std::size_t daggerReadyAnimation{};
        std::size_t daggerAttack1Animation{};
        std::size_t daggerAttack2Animation{};
        std::size_t daggerAttack3Animation{};
        std::size_t daggerAttack4Animation{};
        std::size_t daggerDamageAnimation{};
        std::size_t daggerRunAnimation{};
        // Magic casting
        std::size_t magicReady1Animation{};
        std::size_t magicCast1Animation{};
        std::size_t magicAttack1Animation{};
        std::size_t magicReady2Animation{};
        std::size_t magicCast2Animation{};
        std::size_t magicAttack2Animation{};
        // Buffs
        std::size_t buffReady1Animation{};
        std::size_t buffCast1Animation{};
        std::size_t buffAttack1Animation{};
        std::size_t buffReady2Animation{};
        std::size_t buffCast2Animation{};
        std::size_t buffAttack2Animation{};
        std::size_t buffReady3Animation{};
        std::size_t buffCast3Animation{};
        std::size_t buffAttack3Animation{};
        // Skills
        std::size_t skill1Animation{};
        std::size_t skill2Animation{};
        std::size_t skill3Animation{};
        std::size_t skill4Animation{};
        std::size_t skill5Animation{};
        std::size_t skill6Animation{};
        std::size_t skill7Animation{};
        std::size_t skill8Animation{};
        std::size_t skill9Animation{};
        std::size_t skill10Animation{};
        std::size_t skill11Animation{};
        // Other
        std::size_t stunAnimation{};
        // Emotes
        std::size_t emote1Animation{};
        std::size_t emote2Animation{};
        std::size_t emote3Animation{};
        std::size_t emote4Animation{};
        std::size_t emote5Animation{};
        std::size_t emote6Animation{};
        std::size_t emote7Animation{};
        std::size_t emote8Animation{};
        std::size_t emote9Animation{};
        std::size_t emote10Animation{};

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
        bool preload_items(const std::filesystem::path& dataRoot);
        bool load(const std::filesystem::path& dataRoot);
        // allowPreload=false skips the (heavy, all-races) preload and relies on the
        // per-asset on-demand disk fallback, so the first character can appear before
        // the full caches are built in the background. Subsequent loads (appearance
        // swaps) use the caches once the background preload has completed.
        bool load(const std::filesystem::path& dataRoot, const CharacterAppearance& appearance, bool allowPreload = true);

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

        // BC3 texture cache — pre-converted during preload for instant appearance swaps.
        bool bc3_cache_ready() const { return bc3CacheReady_; }
        const renderer::DdsTexture* bc3_texture_for(const std::filesystem::path& path) const;
        std::uint32_t bc3_target_width() const { return bc3TargetWidth_; }
        std::uint32_t bc3_target_height() const { return bc3TargetHeight_; }
        std::uint32_t bc3_target_mips() const { return bc3TargetMips_; }

        // Character world position.
        float world_x() const { return characterX_; }
        float world_y() const { return characterY_; }
        float world_z() const { return characterZ_; }
        void set_world_position(float x, float y, float z, float yaw = 0.0f);

        // Water tuning — adjustable at runtime via ImGui (absolute world Y; the
        // water surface is Y=0). swimStartY: the character begins swimming once it
        // sinks to/below this height. floatLevelY: the height it bobs up to and
        // floats at while swimming. Kept separate so each can be matched to native.
        float swimStartY{ -1.6f };
        float floatLevelY{ -1.5f };

        // Bone attachment indices — adjustable at runtime via ImGui.
        int weaponBoneIndex{ 11 };   // default: right wrist
        int shieldBoneIndex{ 21 };   // default: left wrist
        int cloakBodyBoneIndex{ 4 };    // default: upper spine/chest
        int cloakShoulderBoneIndex{ 4 }; // default: upper spine/chest
        int mountBoneIndex{ 25 };       // mount bone the rider is seated on
        int animation_bone_count() const;
        int mount_bone_count() const;

        // World-space transform of the equipped weapon's attach bone, recomputed
        // every frame in skin_and_transform(). Used to anchor weapon aura effects.
        // basis columns map weapon-model X/Y/Z axes to world (character scale baked
        // in), so an emitter's local startPosition offset projects correctly.
        struct WeaponAttachment
        {
            bool valid{};
            float position[3]{};
            float basis[9]{};   // [0..2]=Xcol, [3..5]=Ycol, [6..8]=Zcol
        };
        const WeaponAttachment& weapon_attachment() const { return weaponAttachment_; }

    private:
        void skin_and_transform();

        WeaponAttachment weaponAttachment_{};
        CharacterData data_;
        std::vector<CharacterGpuVertex> worldVertices_;
        std::uint32_t textureLayerBase_{};
        bool cacheReady_{};
        std::filesystem::path cachedDataRoot_;
        std::unordered_map<std::string, world::CharacterModel> cachedModels_;
        std::unordered_map<std::string, std::vector<CharacterAnimationChoice>> cachedAnimations_;
        std::unordered_map<std::string, std::uint32_t> cachedTextureSlotByPath_;
        std::vector<std::filesystem::path> cachedTexturePaths_;

        // Pre-converted BC3 textures keyed by normalized path.
        std::unordered_map<std::string, renderer::DdsTexture> cachedBc3Textures_;
        bool bc3CacheReady_{};
        std::uint32_t bc3TargetWidth_{};
        std::uint32_t bc3TargetHeight_{};
        std::uint32_t bc3TargetMips_{};

        // Item (weapon/shield) cache.
        std::unordered_map<std::string, world::ItemModel> cachedItemModels_;
        bool itemCacheReady_{};

        // Playable state.
        float characterX_{};
        float characterY_{};
        float characterZ_{};
        float characterYaw_{};
        float verticalVelocity_{};
        float cameraYaw_{};
        float cameraPitch_{ -0.16f };
        float cameraDistance_{ 6.2f };
        float smoothCameraY_{};   // EMA-smoothed camera target Y (eliminates terrain jitter)
        float animationSeconds_{};
        std::size_t activeAnimation_{};
        std::size_t mountActiveAnimation_{};
        float mountAnimationSeconds_{};
        float mountIdleTimer_{};   // time stationary, for breathing/idle variation
        bool grounded_{ true };
        bool jumpWasDown_{};
        bool groundInitialized_{};

        HeightSampleFn heightFn_{};
        void* heightUserData_{};
        CollisionFn collisionFn_{};
        void* collisionUserData_{};
        bool inWater_{};

        // ---- Cloak cloth simulation (Verlet, world space) ----
        // The body cloak is a regular 5-column x R-row grid (vertex i -> row i/5,
        // col i%5). Per the reversed .PC format, the top row (row 0) is the pinned
        // attachment edge that rides with the character; rows 1..R-1 hang free under
        // gravity, held by distance constraints whose rest lengths are the authored
        // bind grid spacing. Solved with position-based Verlet each frame.
        std::vector<float> clothWorld_;     // 3*N current world positions
        std::vector<float> clothPrev_;      // 3*N previous world positions (Verlet)
        std::vector<float> clothRestUp_;    // per-vertex rest length to row above (world units)
        std::vector<float> clothRestLeft_;  // per-vertex rest length to col to the left
        std::vector<std::uint32_t> clothPinBody_;  // per-column: skinned body vertex the top row follows
        std::vector<float> clothPinOffset_;        // per-column local offset (3 each): cloak bind - body bind
        std::uint32_t clothRows_{};
        static constexpr std::uint32_t kClothCols = 5;
        bool clothInitialized_{};
        float lastDeltaSeconds_{ 1.0f / 60.0f };
        void reset_cloth() { clothInitialized_ = false; }
    };
}
