#pragma once

#include "assets/data_index.h"
#include "character/character_system.h"
#include "character/weapon_effect.h"
#include "effects/effect_system.h"
#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"
#include "ui/editor_panel.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace phoenix::character
{
    using phoenix::ui::CharacterOption;
    inline std::vector<int> scan_csv_indices(const std::filesystem::path& csvPath)
    {
        std::vector<int> indices;
        auto file = phoenix::assets::open_ifstream(csvPath);
        if (!file)
            return indices;
        std::string line;
        std::getline(file, line); // skip header
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::string clean;
            clean.reserve(line.size());
            for (char c : line)
                if (c != '"') clean += c;
            auto comma = clean.find(',');
            if (comma == std::string::npos) continue;
            indices.push_back(std::atoi(clean.substr(0, comma).c_str()));
        }
        std::ranges::sort(indices);
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }
    
    inline std::vector<int> scan_csv_indices_column(const std::filesystem::path& csvPath, std::size_t column)
    {
        std::vector<int> indices;
        auto file = phoenix::assets::open_ifstream(csvPath);
        if (!file)
            return indices;
        std::string line;
        std::getline(file, line); // skip header
        while (std::getline(file, line))
        {
            if (line.empty())
                continue;
            std::string clean;
            clean.reserve(line.size());
            for (char c : line)
                if (c != '"') clean += c;
    
            std::size_t pos = 0;
            std::size_t current = 0;
            while (pos <= clean.size())
            {
                const auto comma = clean.find(',', pos);
                const auto end = comma == std::string::npos ? clean.size() : comma;
                if (current == column)
                {
                    indices.push_back(std::atoi(clean.substr(pos, end - pos).c_str()));
                    break;
                }
                if (comma == std::string::npos)
                    break;
                pos = comma + 1;
                ++current;
            }
        }
        std::ranges::sort(indices);
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    }
    
    inline bool contains_index(const std::vector<int>& values, int wanted)
    {
        return std::ranges::find(values, wanted) != values.end();
    }
    
    inline std::vector<int> common_armor_indices(const CharacterOption& option)
    {
        std::vector<int> indices;
        for (const int index : option.upperIndices)
        {
            if (contains_index(option.lowerIndices, index)
                && contains_index(option.handIndices, index)
                && contains_index(option.footIndices, index))
                indices.push_back(index);
        }
        return indices;
    }
    
    struct BotEquipmentPools
    {
        std::map<phoenix::character::WeaponType, std::vector<int>> itemIndices;
        std::map<std::string, std::vector<int>> cloakIndicesByRace;
        std::map<std::string, std::vector<int>> mountIndicesByClass;
    };
    
    inline BotEquipmentPools scan_bot_equipment_pools(const std::filesystem::path& dataRoot)
    {
        BotEquipmentPools pools;
        const auto itemRoot = phoenix::assets::resolve_existing_path_case_insensitive(dataRoot / "Weapons");
        const auto addItem = [&](phoenix::character::WeaponType type) {
            if (type == phoenix::character::WeaponType::None)
                return;
            const auto typeId = static_cast<int>(type);
            const auto csvPath = itemRoot / std::format("{:02d}.csv", typeId);
            pools.itemIndices[type] = scan_csv_indices_column(csvPath, 2);
        };
    
        addItem(phoenix::character::WeaponType::Sword1H);
        addItem(phoenix::character::WeaponType::Sword2H);
        addItem(phoenix::character::WeaponType::Axe1H);
        addItem(phoenix::character::WeaponType::Axe2H);
        addItem(phoenix::character::WeaponType::DualSword);
        addItem(phoenix::character::WeaponType::Spear);
        addItem(phoenix::character::WeaponType::Mace1H);
        addItem(phoenix::character::WeaponType::Hammer2H);
        addItem(phoenix::character::WeaponType::RevDagger);
        addItem(phoenix::character::WeaponType::Dagger);
        addItem(phoenix::character::WeaponType::Javelin);
        addItem(phoenix::character::WeaponType::Staff);
        addItem(phoenix::character::WeaponType::Bow);
        addItem(phoenix::character::WeaponType::Crossbow);
        addItem(phoenix::character::WeaponType::Claw);
        addItem(phoenix::character::WeaponType::ShieldLight);
        addItem(phoenix::character::WeaponType::ShieldDark);
    
        const auto cloakRoot = phoenix::assets::resolve_existing_path_case_insensitive(dataRoot / "Mantles");
        for (const auto race : { "hu", "de", "el", "vi" })
            pools.cloakIndicesByRace[race] = scan_csv_indices(cloakRoot / ("mantle_" + std::string(race) + ".csv"));
    
        const auto vehicleRoot = phoenix::assets::resolve_existing_path_case_insensitive(dataRoot / "Vehicle");
        for (const auto mountClass : { "hu", "de", "el", "vi" })
            pools.mountIndicesByClass[mountClass] = scan_csv_indices_column(vehicleRoot / ("vehicle_" + std::string(mountClass) + "_01.csv"), 2);
    
        return pools;
    }
    
    inline std::string race_abbrev_for_folder(std::string raceFolder)
    {
        raceFolder = phoenix::assets::lower_ascii(std::move(raceFolder));
        if (raceFolder == "human") return "hu";
        if (raceFolder == "deatheater") return "de";
        if (raceFolder == "elf") return "el";
        if (raceFolder == "vile") return "vi";
        return raceFolder.substr(0, std::min<std::size_t>(2, raceFolder.size()));
    }
    
    // ---- Bot character stress-test system ----
    // Bots are lightweight instances: they share a few skinned pose resources
    // instead of cloning a full CharacterSystem per bot. This keeps the stress
    // test focused on render throughput instead of duplicating skeleton/asset state.
    
    // Packed for cache-friendly iteration.
    struct BotCharacter
    {
        float x{}, y{}, z{}, yaw{};
        float sinYaw{}, cosYaw{ 1.0f };
        float originX{}, originZ{};
        float targetX{}, targetZ{};
        float moveTimer{};
        float actionTimer{};
        float effectTimer{};
        float moveSpeed{};
        std::uint16_t currentAction{};
        std::uint16_t pose{};
        std::uint16_t preset{};
        std::uint8_t fastMove{};
        std::uint8_t visible{};   // set during update, reused for instance building
        std::uint8_t auraPreset{};
    };
    
    // Pose IDs shared across all presets. Each preset may have a subset loaded.
    enum BotPose : std::uint16_t
    {
        kPoseIdle = 0,
        kPoseWalk,
        kPoseRun,
        kPoseJump,
        kPoseSit,
        kPoseDie,
        kPoseAttack1,
        kPoseAttack2,
        kPoseDamage,
        kPoseCast,
        kPoseEmote1,
        kPoseEmote2,
        kPoseEmote3,
        kPoseEmote4,
        kPoseEmote5,
        // Mounted
        kPoseMountIdle,
        kPoseMountRun,
        kPoseCount
    };
    
    struct BotVisualPreset
    {
        phoenix::character::CharacterAppearance appearance;
        std::array<phoenix::character::CharacterSystem, kPoseCount> poses;
        std::array<bool, kPoseCount> poseValid{};
        std::uint32_t textureBase{};
        std::size_t vertexCount{};
        std::size_t indexCount{};
        bool ready{};
        bool mounted{};
    };
    
    struct BotManager
    {
        std::vector<BotCharacter> bots;
        std::mt19937 rng{ std::random_device{}() };
    
        std::vector<BotVisualPreset> visualPresets;
        std::vector<phoenix::character::WeaponEffect> botAuras;
        bool presetsBuilt{};
        bool effectsEnabled{ true };
        bool weaponAurasEnabled{ true };
        std::size_t lastBotCount{};
        std::uint32_t frameCounter{};
    
        // Pending effect spawns from bot update (processed by main loop).
        struct PendingEffect
        {
            float x, y, z;
            std::uint16_t catalogIndex;
        };
        std::vector<PendingEffect> pendingEffects;
        std::vector<std::size_t> oneShotEffectIndices;
    
        static std::size_t poseAnimIndex(BotPose pose, const phoenix::character::CharacterData& d)
        {
            auto first = [](std::initializer_list<std::size_t> anims) -> std::size_t {
                for (auto a : anims) if (a > 0) return a;
                return 0;
            };
            auto weaponRun = [&]() -> std::size_t {
                std::size_t anim = 0;
                switch (d.equippedWeaponType)
                {
                case phoenix::character::WeaponType::Sword2H:
                case phoenix::character::WeaponType::Axe2H:
                case phoenix::character::WeaponType::Hammer2H:
                    anim = d.twoHandRunAnimation; break;
                case phoenix::character::WeaponType::Bow:
                    anim = d.bowRunAnimation; break;
                case phoenix::character::WeaponType::Sword1H:
                case phoenix::character::WeaponType::Axe1H:
                case phoenix::character::WeaponType::Mace1H:
                    anim = d.oneHandRunAnimation; break;
                case phoenix::character::WeaponType::DualSword:
                    anim = d.dualRunAnimation; break;
                case phoenix::character::WeaponType::Spear:
                case phoenix::character::WeaponType::Javelin:
                    anim = d.spearRunAnimation; break;
                case phoenix::character::WeaponType::Crossbow:
                    anim = d.crossbowRunAnimation; break;
                case phoenix::character::WeaponType::Staff:
                    anim = d.staffRunAnimation; break;
                case phoenix::character::WeaponType::RevDagger:
                    anim = d.revDaggerRunAnimation; break;
                case phoenix::character::WeaponType::Claw:
                    anim = d.knuckleRunAnimation; break;
                case phoenix::character::WeaponType::Dagger:
                    anim = d.daggerRunAnimation; break;
                default:
                    break;
                }
                return anim > 0 ? anim : d.runAnimation;
            };
            switch (pose)
            {
            case kPoseIdle:      return d.idleAnimation;
            case kPoseWalk:      return d.walkAnimation;
            case kPoseRun:       return weaponRun();
            case kPoseJump:      return d.jumpAnimation;
            case kPoseSit:       return d.sitAnimation;
            case kPoseDie:       return d.dieAnimation;
            case kPoseAttack1:   return first({d.oneHandAttack1Animation, d.twoHandAttack1Animation,
                                    d.bowAttackAnimation, d.dualAttack1Animation, d.spearAttack1Animation});
            case kPoseAttack2:   return first({d.oneHandAttack2Animation, d.twoHandAttack2Animation,
                                    d.dualAttack2Animation, d.spearAttack2Animation});
            case kPoseDamage:    return first({d.oneHandDamageAnimation, d.twoHandDamageAnimation,
                                    d.bowDamageAnimation, d.dualDamageAnimation});
            case kPoseCast:      return first({d.magicCast1Animation, d.buffCast1Animation,
                                    d.magicCast2Animation});
            case kPoseEmote1:    return d.emote1Animation;
            case kPoseEmote2:    return d.emote2Animation;
            case kPoseEmote3:    return d.emote3Animation;
            case kPoseEmote4:    return d.emote4Animation;
            case kPoseEmote5:    return d.emote5Animation;
            case kPoseMountIdle: return d.vehicleIdleAnimation;
            case kPoseMountRun:  return d.vehicleRun1Animation;
            default:             return d.idleAnimation;
            }
        }
    
        void cacheOneShotEffects()
        {
            if (!oneShotEffectIndices.empty()) return;
            const auto& catalog = phoenix::effects::preset_catalog();
            for (std::size_t i = 0; i < catalog.size(); ++i)
            {
                if (!catalog[i].loop && !catalog[i].projectile)
                    oneShotEffectIndices.push_back(i);
            }
        }
    
        std::vector<phoenix::renderer::TerrainVertex> poseVertices;
        std::vector<std::uint32_t> poseIndices;
        std::vector<phoenix::renderer::ObjectInstance> poseInstances;
        std::vector<phoenix::renderer::ObjectBatch> poseBatches;
        std::vector<std::vector<phoenix::renderer::ObjectInstance>> poseInstanceBuckets;
        std::vector<std::uint32_t> poseFirstIndices;
        std::vector<std::uint32_t> poseIndexCounts;
        std::vector<std::size_t> poseVertexOffsets;
        std::vector<std::size_t> poseVertexCounts;
        std::vector<std::uint8_t> activePoseMask;
        std::size_t previousPlayerVertCount{};
        std::size_t previousPlayerIndexCount{};
        std::size_t poseVertexCount{};
        std::size_t poseIndexCount{};
        float poseUpdateAccumulator{};
        bool combinedTopologyDirty{ true };
        bool poseMeshUploaded{};
        bool poseVerticesDirty{ true };
    
        float randomFloat(float lo, float hi)
        {
            return std::uniform_real_distribution<float>(lo, hi)(rng);
        }
        int randomInt(int lo, int hi)
        {
            return std::uniform_int_distribution<int>(lo, hi)(rng);
        }

        static float approach(float value, float target, float delta)
        {
            if (value < target)
                return std::min(value + delta, target);
            return std::max(value - delta, target);
        }

        static float wrap_angle(float angle)
        {
            constexpr float kPi = 3.14159265358979323846f;
            while (angle > kPi) angle -= kPi * 2.0f;
            while (angle < -kPi) angle += kPi * 2.0f;
            return angle;
        }
    
        template <typename T>
        const T& randomChoice(const std::vector<T>& values)
        {
            return values[static_cast<std::size_t>(randomInt(0, static_cast<int>(values.size()) - 1))];
        }
    
        float poseUpdateInterval() const
        {
            const auto count = bots.size();
            if (count <= 32) return 1.0f / 12.0f;
            if (count <= 128) return 1.0f / 18.0f;
            if (count <= 512) return 1.0f / 24.0f;
            return 1.0f / 30.0f;
        }

        std::uint16_t random_preset_index(bool preferMounted)
        {
            std::uint16_t chosen = 0;
            int matches = 0;
            for (std::size_t i = 0; i < visualPresets.size(); ++i)
            {
                if (visualPresets[i].mounted != preferMounted)
                    continue;
                ++matches;
                if (randomInt(1, matches) == 1)
                    chosen = static_cast<std::uint16_t>(i);
            }
            if (matches > 0)
                return chosen;
            return static_cast<std::uint16_t>(randomInt(0, static_cast<int>(visualPresets.size()) - 1));
        }

        static int mount_bone_for_race(const std::string& raceAbbrev)
        {
            if (raceAbbrev == "de") return 34;
            if (raceAbbrev == "el") return 19;
            if (raceAbbrev == "vi") return 19;
            return 25;
        }

        static bool weapon_can_carry_shield(phoenix::character::WeaponType type)
        {
            switch (type)
            {
            case phoenix::character::WeaponType::Sword1H:
            case phoenix::character::WeaponType::Axe1H:
            case phoenix::character::WeaponType::Mace1H:
            case phoenix::character::WeaponType::RevDagger:
            case phoenix::character::WeaponType::Dagger:
            case phoenix::character::WeaponType::Javelin:
                return true;
            default:
                return false;
            }
        }
    
        bool build_random_presets(
            const std::filesystem::path& dataRoot,
            const std::vector<CharacterOption>& characterOptions,
            const BotEquipmentPools& equipmentPools,
            std::uint32_t firstTextureSlot,
            std::uint32_t maxTextureSlots,
            std::vector<phoenix::renderer::DdsTexture>& textureSlots,
            bool allowPreload)
        {
            if (characterOptions.empty() || maxTextureSlots == 0)
                return false;
    
            visualPresets.clear();
            presetsBuilt = false;
            std::uint32_t nextTextureSlot = firstTextureSlot;
            const auto endTextureSlot = firstTextureSlot + maxTextureSlots;
            constexpr std::size_t kTargetPresetCount = 14;
            constexpr std::size_t kTargetMountedPresetCount = 4;
    
            static constexpr phoenix::character::WeaponType weaponTypes[] = {
                phoenix::character::WeaponType::Sword1H,
                phoenix::character::WeaponType::Sword2H,
                phoenix::character::WeaponType::Axe1H,
                phoenix::character::WeaponType::Axe2H,
                phoenix::character::WeaponType::DualSword,
                phoenix::character::WeaponType::Spear,
                phoenix::character::WeaponType::Mace1H,
                phoenix::character::WeaponType::Hammer2H,
                phoenix::character::WeaponType::RevDagger,
                phoenix::character::WeaponType::Dagger,
                phoenix::character::WeaponType::Javelin,
                phoenix::character::WeaponType::Staff,
                phoenix::character::WeaponType::Bow,
                phoenix::character::WeaponType::Crossbow,
                phoenix::character::WeaponType::Claw,
            };

            std::vector<phoenix::character::WeaponType> availableWeaponTypes;
            availableWeaponTypes.reserve(std::size(weaponTypes));
            for (const auto type : weaponTypes)
            {
                if (const auto it = equipmentPools.itemIndices.find(type);
                    it != equipmentPools.itemIndices.end() && !it->second.empty())
                    availableWeaponTypes.push_back(type);
            }
            if (availableWeaponTypes.empty())
                return false;
    
            for (std::size_t attempt = 0; attempt < kTargetPresetCount * 8 && visualPresets.size() < kTargetPresetCount; ++attempt)
            {
                const auto& option = randomChoice(characterOptions);
                auto armorIndices = common_armor_indices(option);
                std::erase(armorIndices, 1);
                if (armorIndices.empty() || option.faceIndices.empty() || option.hairIndices.empty())
                    continue;
    
                phoenix::character::CharacterAppearance appearance{};
                appearance.raceFolder = option.raceFolder;
                appearance.prefix = option.prefix;
                const int armor = randomChoice(armorIndices);
                appearance.upperIndex = armor;
                appearance.lowerIndex = armor;
                appearance.handIndex = armor;
                appearance.footIndex = armor;
                appearance.faceIndex = randomChoice(option.faceIndices);
                appearance.hairIndex = randomChoice(option.hairIndices);
                appearance.helmetVisible = false;
                appearance.helmetIndex = -1;
    
                const auto raceAbbrev = race_abbrev_for_folder(appearance.raceFolder);
                if (randomInt(0, 4) == 0)
                {
                    if (const auto it = equipmentPools.cloakIndicesByRace.find(raceAbbrev);
                        it != equipmentPools.cloakIndicesByRace.end() && !it->second.empty())
                        appearance.cloakIndex = randomChoice(it->second);
                    else
                        appearance.cloakIndex = -1;
                }
                else
                {
                    appearance.cloakIndex = -1;
                }
    
                appearance.weaponType = randomChoice(availableWeaponTypes);
                appearance.weaponIndex = randomChoice(equipmentPools.itemIndices.at(appearance.weaponType));
    
                const bool darkRace = raceAbbrev == "de" || raceAbbrev == "vi";
                appearance.shieldType = weapon_can_carry_shield(appearance.weaponType)
                    ? (darkRace ? phoenix::character::WeaponType::ShieldDark : phoenix::character::WeaponType::ShieldLight)
                    : phoenix::character::WeaponType::None;
                appearance.shieldIndex = -1;
                if (appearance.shieldType != phoenix::character::WeaponType::None)
                {
                    if (const auto it = equipmentPools.itemIndices.find(appearance.shieldType);
                        it != equipmentPools.itemIndices.end() && !it->second.empty())
                        appearance.shieldIndex = randomChoice(it->second);
                    else
                        appearance.shieldType = phoenix::character::WeaponType::None;
                }
    
                const auto mountedPresetCount = static_cast<std::size_t>(std::ranges::count_if(visualPresets, [](const auto& preset) {
                    return preset.mounted;
                }));
                const auto remainingSlots = kTargetPresetCount - visualPresets.size();
                const auto mountedStillNeeded = mountedPresetCount < kTargetMountedPresetCount
                    ? kTargetMountedPresetCount - mountedPresetCount
                    : 0;
                const bool forceMountedPreset = mountedStillNeeded > 0 && remainingSlots <= mountedStillNeeded;
                const bool shouldBuildMountedPreset = forceMountedPreset
                    || (mountedStillNeeded > 0 && randomInt(1, 4) == 1);
                if (shouldBuildMountedPreset)
                {
                    if (const auto it = equipmentPools.mountIndicesByClass.find(raceAbbrev);
                        it != equipmentPools.mountIndicesByClass.end() && !it->second.empty())
                    {
                        appearance.mounted = true;
                        appearance.mountClass = raceAbbrev;
                        appearance.mountIndex = it->second.front();
                    }
                    else if (forceMountedPreset)
                    {
                        continue;
                    }
                }
    
                BotVisualPreset preset{};
                preset.appearance = appearance;
                preset.textureBase = nextTextureSlot;
                preset.mounted = appearance.mounted;
                const int mountBoneIndex = mount_bone_for_race(raceAbbrev);
                preset.poses[kPoseIdle].mountBoneIndex = mountBoneIndex;
    
                // Load into pose slot 0 (idle) — all others clone from it.
                if (!preset.poses[kPoseIdle].load(dataRoot, appearance, allowPreload)
                    || !preset.poses[kPoseIdle].ready())
                    continue;
    
                const auto textureCount = static_cast<std::uint32_t>(preset.poses[kPoseIdle].texture_paths().size());
                if (textureCount == 0 || nextTextureSlot + textureCount > endTextureSlot)
                    break;
    
                preset.poses[kPoseIdle].set_texture_layer_base(preset.textureBase);
                const auto& texPaths = preset.poses[kPoseIdle].texture_paths();
                bool allTexturesValid = true;
                for (std::uint32_t i = 0; i < textureCount; ++i)
                {
                    // Use BC3 cache when available (correct format, no disk I/O).
                    const auto* cached = preset.poses[kPoseIdle].bc3_texture_for(texPaths[i]);
                    if (cached)
                        textureSlots[preset.textureBase + i] = *cached;
                    else
                        textureSlots[preset.textureBase + i] = phoenix::renderer::load_dds(texPaths[i]);
                    if (!textureSlots[preset.textureBase + i].valid)
                        allTexturesValid = false;
                }
                if (!allTexturesValid)
                    continue;
    
                // Clone all pose slots from the loaded one.
                for (std::size_t p = 1; p < kPoseCount; ++p)
                {
                    preset.poses[p].clone_from(preset.poses[kPoseIdle]);
                    preset.poses[p].mountBoneIndex = mountBoneIndex;
                    preset.poses[p].set_texture_layer_base(preset.textureBase);
                    preset.poses[p].set_world_position(0.0f, 0.0f, 0.0f, 0.0f);
                }
                preset.poses[kPoseIdle].set_world_position(0.0f, 0.0f, 0.0f, 0.0f);
    
                // Determine which poses are valid based on available animations.
                const auto& d = preset.poses[kPoseIdle].character_data();
                auto valid = [&](std::size_t animIdx) {
                    return animIdx > 0 && animIdx < d.animations.size()
                        && d.animations[animIdx].animation.parsed;
                };
                preset.poseValid[kPoseIdle]    = valid(d.idleAnimation);
                preset.poseValid[kPoseWalk]    = valid(d.walkAnimation);
                preset.poseValid[kPoseRun]     = valid(d.runAnimation)
                    || valid(d.oneHandRunAnimation) || valid(d.twoHandRunAnimation)
                    || valid(d.bowRunAnimation) || valid(d.dualRunAnimation)
                    || valid(d.spearRunAnimation) || valid(d.crossbowRunAnimation)
                    || valid(d.staffRunAnimation) || valid(d.revDaggerRunAnimation)
                    || valid(d.knuckleRunAnimation) || valid(d.daggerRunAnimation);
                preset.poseValid[kPoseJump]    = valid(d.jumpAnimation);
                preset.poseValid[kPoseSit]     = valid(d.sitAnimation);
                preset.poseValid[kPoseDie]     = valid(d.dieAnimation);
                preset.poseValid[kPoseEmote1]  = valid(d.emote1Animation);
                preset.poseValid[kPoseEmote2]  = valid(d.emote2Animation);
                preset.poseValid[kPoseEmote3]  = valid(d.emote3Animation);
                preset.poseValid[kPoseEmote4]  = valid(d.emote4Animation);
                preset.poseValid[kPoseEmote5]  = valid(d.emote5Animation);
                preset.poseValid[kPoseMountIdle] = preset.mounted && valid(d.vehicleIdleAnimation);
                preset.poseValid[kPoseMountRun]  = preset.mounted && valid(d.vehicleRun1Animation);
    
                // Weapon-specific attack/damage/cast.
                preset.poseValid[kPoseAttack1] = valid(d.oneHandAttack1Animation)
                    || valid(d.twoHandAttack1Animation) || valid(d.bowAttackAnimation)
                    || valid(d.dualAttack1Animation) || valid(d.spearAttack1Animation);
                preset.poseValid[kPoseAttack2] = valid(d.oneHandAttack2Animation)
                    || valid(d.twoHandAttack2Animation) || valid(d.dualAttack2Animation);
                preset.poseValid[kPoseDamage] = valid(d.oneHandDamageAnimation)
                    || valid(d.twoHandDamageAnimation) || valid(d.bowDamageAnimation);
                preset.poseValid[kPoseCast] = valid(d.magicCast1Animation)
                    || valid(d.buffCast1Animation);
    
                preset.vertexCount = preset.poses[kPoseIdle].world_vertices().size();
                preset.indexCount = preset.poses[kPoseIdle].indices().size();
                preset.ready = preset.vertexCount > 0 && preset.indexCount > 0;
                if (!preset.ready)
                    continue;
    
                nextTextureSlot += textureCount;
                visualPresets.push_back(std::move(preset));
            }
    
            presetsBuilt = !visualPresets.empty();
            poseVerticesDirty = true;
            poseMeshUploaded = false;
            return presetsBuilt;
        }
    
        void spawn(int count, float centerX, float centerZ,
            phoenix::character::HeightSampleFn heightFn, void* heightUserData)
        {
            if (!presetsBuilt || visualPresets.empty())
                return;
            bots.reserve(bots.size() + count);
            botAuras.reserve(botAuras.size() + static_cast<std::size_t>(std::max(0, count)));
            for (int i = 0; i < count; ++i)
            {
                BotCharacter bot{};
                bot.originX = centerX + randomFloat(-50.0f, 50.0f);
                bot.originZ = centerZ + randomFloat(-50.0f, 50.0f);
                bot.targetX = bot.originX + randomFloat(-30.0f, 30.0f);
                bot.targetZ = bot.originZ + randomFloat(-30.0f, 30.0f);
                float groundY = heightFn ? heightFn(bot.originX, bot.originZ, heightUserData) : 0.0f;
                bot.x = bot.originX;
                bot.y = groundY;
                bot.z = bot.originZ;
                bot.yaw = randomFloat(-3.14f, 3.14f);
                bot.sinYaw = std::sin(bot.yaw);
                bot.cosYaw = std::cos(bot.yaw);
                bot.moveTimer = randomFloat(1.0f, 4.0f);
                bot.actionTimer = randomFloat(4.0f, 12.0f);
                bot.effectTimer = randomFloat(0.5f, 6.0f);
                bot.moveSpeed = 0.0f;
                bot.currentAction = 1;
                bot.pose = 1;
                bot.preset = random_preset_index(randomInt(1, 10) == 1);
                bot.fastMove = randomInt(0, 2) == 0 ? 1 : 0;
                bot.auraPreset = static_cast<std::uint8_t>(randomInt(0, 5));
                bots.push_back(bot);

                phoenix::character::WeaponEffect aura;
                aura.enabled() = true;
                aura.apply_preset(0, static_cast<phoenix::character::WeaponEffect::Preset>(bot.auraPreset));
                botAuras.push_back(std::move(aura));
            }
        }
    
        void clear_bots()
        {
            bots.clear(); bots.shrink_to_fit();
            botAuras.clear(); botAuras.shrink_to_fit();
            lastBotCount = 0;
            poseInstances.clear(); poseInstances.shrink_to_fit();
            poseBatches.clear(); poseBatches.shrink_to_fit();
            poseInstanceBuckets.clear(); poseInstanceBuckets.shrink_to_fit();
            activePoseMask.clear(); activePoseMask.shrink_to_fit();
            pendingEffects.clear(); pendingEffects.shrink_to_fit();
            poseUpdateAccumulator = 0.0f;
            poseVerticesDirty = true;
        }

        void clear()
        {
            clear_bots();
            visualPresets.clear(); visualPresets.shrink_to_fit();
            presetsBuilt = false;
            poseVertices.clear(); poseVertices.shrink_to_fit();
            poseIndices.clear(); poseIndices.shrink_to_fit();
            poseFirstIndices.clear(); poseFirstIndices.shrink_to_fit();
            poseIndexCounts.clear(); poseIndexCounts.shrink_to_fit();
            poseVertexOffsets.clear(); poseVertexOffsets.shrink_to_fit();
            poseVertexCounts.clear(); poseVertexCounts.shrink_to_fit();
            previousPlayerVertCount = 0;
            previousPlayerIndexCount = 0;
            poseVertexCount = 0;
            poseIndexCount = 0;
            poseUpdateAccumulator = 0.0f;
            combinedTopologyDirty = true;
            poseMeshUploaded = false;
            poseVerticesDirty = true;
        }
    
        void update(float dt, float camX, float camZ, float cullDist,
            phoenix::character::HeightSampleFn heightFn, void* heightUserData)
        {
            pendingEffects.clear();
            if (bots.empty())
                return;
            if (!presetsBuilt || visualPresets.empty())
            {
                clear_bots();
                return;
            }
    
            ++frameCounter;
            const float cullDistSq = cullDist * cullDist;
            cacheOneShotEffects();
            activePoseMask.assign(visualPresets.size() * kPoseCount, 0);
    
            const auto botCount = bots.size();
            auto* botData = bots.data();
            for (std::size_t bi = 0; bi < botCount; ++bi)
            {
                auto& bot = botData[bi];
                const float dx = bot.x - camX;
                const float dz = bot.z - camZ;
                const float distSq = dx * dx + dz * dz;
                bot.visible = distSq <= cullDistSq ? 1 : 0;
                if (!bot.visible) continue;
    
                bot.actionTimer -= dt;
    
                const auto pi = std::min<std::uint16_t>(bot.preset,
                    visualPresets.empty() ? 0 : static_cast<std::uint16_t>(visualPresets.size() - 1));
                const bool isMounted = !visualPresets.empty() && visualPresets[pi].mounted;
    
                if (bot.currentAction == 0)
                    bot.moveTimer -= dt;

                if (bot.currentAction == 0 && bot.moveTimer <= 0.0f)
                {
                    bot.targetX = bot.originX + randomFloat(-55.0f, 55.0f);
                    bot.targetZ = bot.originZ + randomFloat(-55.0f, 55.0f);
                    bot.moveTimer = randomFloat(0.35f, 1.2f);
                    bot.fastMove = randomInt(0, 99) < 35 ? 1 : 0;
                    bot.currentAction = 1;
                }

                if (bot.currentAction == 0 && bot.actionTimer <= 0.0f)
                {
                    // 0=idle, 1=move, 2=attack, 3=cast, 4=emote, 5=jump, 6=die, 7=damage, 8=sit
                    static constexpr std::uint16_t idleActions[] = { 2, 3, 4, 4, 5, 7, 8 };
                    bot.currentAction = idleActions[static_cast<std::size_t>(randomInt(0, static_cast<int>(std::size(idleActions)) - 1))];
                    bot.actionTimer = randomFloat(5.0f, 14.0f);
                }
                if (bot.currentAction == 0)
                    bot.moveSpeed = approach(bot.moveSpeed, 0.0f, 10.0f * dt);
    
                bot.pose = isMounted ? kPoseMountIdle : kPoseIdle;
                switch (bot.currentAction)
                {
                case 1: {
                    const float tdx = bot.targetX - bot.x;
                    const float tdz = bot.targetZ - bot.z;
                    if (tdx * tdx + tdz * tdz > 1.0f)
                    {
                        const float targetYaw = std::atan2(tdx, tdz);
                        const float turnRate = bot.fastMove ? 5.0f : 3.8f;
                        bot.yaw += std::clamp(wrap_angle(targetYaw - bot.yaw), -turnRate * dt, turnRate * dt);
                        bot.sinYaw = std::sin(bot.yaw);
                        bot.cosYaw = std::cos(bot.yaw);
                        const float targetSpeed = bot.fastMove ? (isMounted ? 7.5f : 6.0f) : 3.0f;
                        bot.moveSpeed = approach(bot.moveSpeed, targetSpeed, 8.0f * dt);
                        bot.x += bot.sinYaw * bot.moveSpeed * dt;
                        bot.z += bot.cosYaw * bot.moveSpeed * dt;
                        if (heightFn)
                        {
                            const bool nearBot = distSq < 2500.0f;
                            if (nearBot || (frameCounter & 3u) == (bi & 3u))
                                bot.y = heightFn(bot.x, bot.z, heightUserData);
                        }
                        if (isMounted)
                            bot.pose = kPoseMountRun;
                        else
                            bot.pose = bot.fastMove ? kPoseRun : kPoseWalk;
                    }
                    else
                    {
                        bot.moveSpeed = approach(bot.moveSpeed, 0.0f, 10.0f * dt);
                        bot.currentAction = 0;
                        bot.moveTimer = randomFloat(0.45f, 1.5f);
                    }
                    break;
                }
                case 2: bot.pose = kPoseAttack1 + static_cast<std::uint16_t>(randomInt(0, 1)); bot.currentAction = 0; break;
                case 3: bot.pose = kPoseCast; bot.currentAction = 0; break;
                case 4: bot.pose = static_cast<std::uint16_t>(kPoseEmote1 + randomInt(0, 4)); bot.currentAction = 0; break;
                case 5: bot.pose = kPoseJump; bot.currentAction = 0; break;
                case 6: bot.pose = kPoseDie; bot.currentAction = 0; break;
                case 7: bot.pose = kPoseDamage; bot.currentAction = 0; break;
                case 8: bot.pose = kPoseSit; bot.currentAction = 0; break;
                default: break;
                }
                // Fall back to idle if the chosen pose isn't available for this preset.
                if (!visualPresets.empty() && !visualPresets[pi].poseValid[bot.pose])
                    bot.pose = isMounted ? kPoseMountIdle : kPoseIdle;
    
                // Random one-shot effect (only near bots, capped per frame).
                bot.effectTimer -= dt;
                if (effectsEnabled && bot.effectTimer <= 0.0f && distSq < 6400.0f
                    && !oneShotEffectIndices.empty() && pendingEffects.size() < 8)
                {
                    bot.effectTimer = randomFloat(1.5f, 8.0f);
                    PendingEffect pe{};
                    pe.x = bot.x; pe.y = bot.y; pe.z = bot.z;
                    pe.catalogIndex = static_cast<std::uint16_t>(
                        oneShotEffectIndices[static_cast<std::size_t>(
                            randomInt(0, static_cast<int>(oneShotEffectIndices.size()) - 1))]);
                    pendingEffects.push_back(pe);
                }
    
                if (!visualPresets.empty())
                {
                    const auto poseIdx = std::min<std::uint16_t>(bot.pose, kPoseCount - 1);
                    activePoseMask[static_cast<std::size_t>(pi) * kPoseCount + poseIdx] = 1;
                }
            }
    
            if (!visualPresets.empty())
            {
                poseUpdateAccumulator += dt;
                const auto interval = poseUpdateInterval();
                if (poseVerticesDirty || poseUpdateAccumulator >= interval)
                {
                    const auto poseDt = poseVerticesDirty
                        ? std::min(dt, interval)
                        : poseUpdateAccumulator;
                    poseUpdateAccumulator = 0.0f;
    
                    // Collect active poses to skin.
                    struct PoseSkinWork
                    {
                        phoenix::character::CharacterSystem* pose;
                        std::size_t animIdx;
                    };
                    std::vector<PoseSkinWork> skinWork;
                    skinWork.reserve(32);
    
                    for (std::size_t presetIndex = 0; presetIndex < visualPresets.size(); ++presetIndex)
                    {
                        auto& preset = visualPresets[presetIndex];
                        const auto& data = preset.poses[kPoseIdle].character_data();
                        for (std::size_t poseIndex = 0; poseIndex < kPoseCount; ++poseIndex)
                        {
                            const auto maskIndex = presetIndex * kPoseCount + poseIndex;
                            if (maskIndex >= activePoseMask.size() || activePoseMask[maskIndex] == 0)
                                continue;
                            if (!preset.poseValid[poseIndex])
                                continue;
                            const auto animIdx = poseAnimIndex(static_cast<BotPose>(poseIndex), data);
                            skinWork.push_back({ &preset.poses[poseIndex], animIdx });
                        }
                    }
    
                    // Parallel pose skinning across all CPU cores.
                    const auto workCount = skinWork.size();
                    const auto threadCount = std::max(1u, std::thread::hardware_concurrency());
                    if (workCount <= 2 || threadCount <= 1)
                    {
                        for (auto& w : skinWork)
                            w.pose->advance_pose(poseDt, w.animIdx);
                    }
                    else
                    {
                        std::atomic<std::size_t> nextIdx{ 0 };
                        auto worker = [&]() {
                            for (;;)
                            {
                                const auto i = nextIdx.fetch_add(1, std::memory_order_relaxed);
                                if (i >= workCount) break;
                                skinWork[i].pose->advance_pose(poseDt, skinWork[i].animIdx);
                            }
                        };
                        std::vector<std::thread> threads;
                        threads.reserve(threadCount - 1);
                        for (std::uint32_t t = 1; t < threadCount; ++t)
                            threads.emplace_back(worker);
                        worker();
                        for (auto& t : threads) t.join();
                    }
    
                    poseVerticesDirty = true;
                }
            }
        }
    
        bool updatePoseMesh(phoenix::renderer::VulkanRenderer& renderer)
        {
            if (visualPresets.empty())
                return false;
    
            if (poseMeshUploaded && !poseVerticesDirty)
                return true;
    
            const auto expectedSlots = visualPresets.size() * kPoseCount;
            const bool topologyChanged = !poseMeshUploaded
                || poseFirstIndices.size() != expectedSlots
                || poseVertexOffsets.size() != expectedSlots;
    
            if (topologyChanged)
            {
                poseVertices.clear();
                poseIndices.clear();
                poseFirstIndices.clear();
                poseIndexCounts.clear();
                poseVertexOffsets.clear();
                poseVertexCounts.clear();
                const auto slotCount = visualPresets.size() * kPoseCount;
                poseFirstIndices.reserve(slotCount);
                poseIndexCounts.reserve(slotCount);
                poseVertexOffsets.reserve(slotCount);
                poseVertexCounts.reserve(slotCount);
            }
    
            std::size_t poseSlot = 0;
            for (const auto& preset : visualPresets)
            {
                for (const auto& poseCharacter : preset.poses)
                {
                    const auto& verts = poseCharacter.world_vertices();
                    const auto* tv = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(verts.data());
                    if (topologyChanged)
                    {
                        const auto vertexOffset = static_cast<std::uint32_t>(poseVertices.size());
                        poseVertexOffsets.push_back(poseVertices.size());
                        poseVertexCounts.push_back(verts.size());
                        poseVertices.insert(poseVertices.end(), tv, tv + verts.size());
                        const auto firstIndex = static_cast<std::uint32_t>(poseIndices.size());
                        for (auto idx : poseCharacter.indices())
                            poseIndices.push_back(idx + vertexOffset);
                        poseFirstIndices.push_back(firstIndex);
                        poseIndexCounts.push_back(static_cast<std::uint32_t>(poseCharacter.indices().size()));
                    }
                    else if (poseSlot < activePoseMask.size() && activePoseMask[poseSlot] != 0
                        && poseSlot < poseVertexOffsets.size() && poseSlot < poseVertexCounts.size())
                    {
                        const auto dstOffset = poseVertexOffsets[poseSlot];
                        const auto dstCount = std::min<std::size_t>(poseVertexCounts[poseSlot], verts.size());
                        if (dstOffset + dstCount <= poseVertices.size())
                            std::memcpy(poseVertices.data() + dstOffset, tv, dstCount * sizeof(phoenix::renderer::TerrainVertex));
                    }
                    ++poseSlot;
                }
            }
    
            poseVertexCount = poseVertices.size();
            poseIndexCount = poseIndices.size();
            if (topologyChanged)
            {
                poseMeshUploaded = renderer.set_bot_character_mesh(poseVertices, poseIndices);
                poseVerticesDirty = !poseMeshUploaded;
                return poseMeshUploaded;
            }
    
            const auto updated = renderer.update_bot_character_vertices(poseVertices);
            poseVerticesDirty = !updated;
            return updated;
        }
    
        bool updatePoseInstances(
            phoenix::renderer::VulkanRenderer& renderer)
        {
            if (!poseMeshUploaded || visualPresets.empty())
                return false;
    
            const auto batchCount = visualPresets.size() * kPoseCount;
            if (poseFirstIndices.size() != batchCount || poseIndexCounts.size() != batchCount)
                return false;
            if (poseInstanceBuckets.size() != batchCount)
                poseInstanceBuckets.resize(batchCount);
            for (auto& bucket : poseInstanceBuckets)
                bucket.clear();
    
            // Use cached visibility + sin/cos from update() — no distance check or trig here.
            const auto botCount = bots.size();
            const auto* botData = bots.data();
            const auto presetMax = visualPresets.empty()
                ? static_cast<std::uint16_t>(0)
                : static_cast<std::uint16_t>(visualPresets.size() - 1);
            for (std::size_t bi = 0; bi < botCount; ++bi)
            {
                const auto& bot = botData[bi];
                if (!bot.visible) continue;
    
                phoenix::renderer::ObjectInstance inst{};
                inst.right[0] = bot.cosYaw;  inst.right[2] = -bot.sinYaw;
                inst.up[1] = 1.0f;
                inst.forward[0] = bot.sinYaw; inst.forward[2] = bot.cosYaw;
                inst.position[0] = bot.x;
                inst.position[1] = bot.y;
                inst.position[2] = bot.z;
                inst.position[3] = 1.0f;
                const auto batchIndex = static_cast<std::size_t>(std::min(bot.preset, presetMax)) * kPoseCount
                    + std::min<std::uint16_t>(bot.pose, kPoseCount - 1);
                poseInstanceBuckets[batchIndex].push_back(inst);
            }
    
            poseInstances.clear();
            poseBatches.clear();
            std::size_t totalInstances = 0;
            for (const auto& bucket : poseInstanceBuckets)
                totalInstances += bucket.size();
            poseInstances.reserve(totalInstances);
            poseBatches.reserve(32);
    
            // Only emit batches that have at least one instance — avoids sending
            // 100+ empty draw calls to the GPU.
            for (std::size_t batchIndex = 0; batchIndex < batchCount; ++batchIndex)
            {
                const auto& bucket = poseInstanceBuckets[batchIndex];
                if (bucket.empty()) continue;
                phoenix::renderer::ObjectBatch batch{};
                batch.firstIndex = poseFirstIndices[batchIndex];
                batch.indexCount = poseIndexCounts[batchIndex];
                batch.firstInstance = static_cast<std::uint32_t>(poseInstances.size());
                batch.instanceCount = static_cast<std::uint32_t>(bucket.size());
                poseInstances.insert(poseInstances.end(), bucket.begin(), bucket.end());
                poseBatches.push_back(batch);
            }
    
            return renderer.update_bot_character_instances(poseInstances, poseBatches);
        }

        static phoenix::character::CharacterSystem::WeaponAttachment transform_bot_attachment(
            const phoenix::character::CharacterSystem::WeaponAttachment& localAttach,
            const BotCharacter& bot)
        {
            phoenix::character::CharacterSystem::WeaponAttachment worldAttach = localAttach;
            worldAttach.position[0] = bot.x + bot.cosYaw * localAttach.position[0] + bot.sinYaw * localAttach.position[2];
            worldAttach.position[1] = bot.y + localAttach.position[1];
            worldAttach.position[2] = bot.z - bot.sinYaw * localAttach.position[0] + bot.cosYaw * localAttach.position[2];

            for (int axis = 0; axis < 3; ++axis)
            {
                const float x = localAttach.basis[axis * 3 + 0];
                const float y = localAttach.basis[axis * 3 + 1];
                const float z = localAttach.basis[axis * 3 + 2];
                worldAttach.basis[axis * 3 + 0] = bot.cosYaw * x + bot.sinYaw * z;
                worldAttach.basis[axis * 3 + 1] = y;
                worldAttach.basis[axis * 3 + 2] = -bot.sinYaw * x + bot.cosYaw * z;
            }
            return worldAttach;
        }

        void emit_weapon_auras(float dt, phoenix::renderer::ParticleBatch& batch)
        {
            if (!presetsBuilt || visualPresets.empty() || botAuras.empty())
                return;

            phoenix::character::CharacterSystem::WeaponAttachment invalidAttach{};
            if (!weaponAurasEnabled)
            {
                for (auto& aura : botAuras)
                {
                    aura.enabled() = false;
                    aura.update(0.0f, invalidAttach, batch);
                }
                return;
            }

            const auto auraCount = std::min(botAuras.size(), bots.size());
            for (std::size_t bi = 0; bi < auraCount; ++bi)
            {
                const auto& bot = bots[bi];
                if (!bot.visible)
                    continue;
                const auto presetIndex = static_cast<std::size_t>(std::min<std::uint16_t>(
                    bot.preset, static_cast<std::uint16_t>(visualPresets.size() - 1)));
                const auto& preset = visualPresets[presetIndex];
                if (preset.mounted)
                    continue;

                const auto poseIndex = static_cast<std::size_t>(std::min<std::uint16_t>(bot.pose, kPoseCount - 1));
                if (!preset.poseValid[poseIndex])
                    continue;
                const auto& localAttach = preset.poses[poseIndex].weapon_attachment();
                if (!localAttach.valid)
                    continue;

                auto worldAttach = transform_bot_attachment(localAttach, bot);
                auto& aura = botAuras[bi];
                aura.enabled() = true;
                aura.update(dt, worldAttach, batch);
            }
        }
    };
    
    
}
