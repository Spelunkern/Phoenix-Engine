#include "ui/editor_panel.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace phoenix::ui
{
    namespace
    {
        constexpr float kFogStartRatio = 0.38f;   // fog begins at 38% of viewDistance
        constexpr float kFogEndRatio = 0.82f;     // fog is ~100% opaque at 82% of viewDistance
    }

// === moved bodies appended below by build step ===
    float apply_renderer_fog(
        phoenix::renderer::VulkanRenderer& renderer,
        const phoenix::runtime::PhoenixRuntime& runtime,
        bool fogEnabled,
        float viewDistance,
        WeatherMode weatherMode)
    {
        const auto& world = runtime.state().world;
        // Dungeons always use pitch-black sky and fog — no clouds, no sky texture.
        const bool dungeon = world.isDungeon;
        std::array<float, 3> weatherFog{
            dungeon ? 0.0f : world.fogColor[0],
            dungeon ? 0.0f : world.fogColor[1],
            dungeon ? 0.0f : world.fogColor[2],
        };
        std::array<float, 12> skyTuning{
            0.62f, 0.0f, 1.0f, 0.0f,
            1.35f, 0.78f, 0.34f, 0.12f,
            0.82f, 1.10f, 0.22f, 0.06f,
        };
        if (weatherMode == WeatherMode::Storm)
        {
            weatherFog = { 0.20f, 0.22f, 0.25f };
            skyTuning[3] = 1.0f;
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            weatherFog = { 0.55f, 0.57f, 0.60f };
            skyTuning[3] = 2.0f;
        }
        else if (weatherMode == WeatherMode::Sunset)
        {
            weatherFog = { 0.78f, 0.42f, 0.24f };
            skyTuning[3] = 3.0f;
        }
        else if (weatherMode == WeatherMode::Night)
        {
            weatherFog = { 0.035f, 0.045f, 0.075f };
            skyTuning[3] = 4.0f;
        }
        renderer.set_sky_tuning(skyTuning.data(), static_cast<std::uint32_t>(skyTuning.size()));

        if (!fogEnabled && !dungeon)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                100000.0f,
                100001.0f,
                world.parsedSky && !world.skyFileName.empty());
            return viewDistance;
        }
        // Dungeons always have fog (black) even when the user disables the fog
        // checkbox, because the alternative is seeing the skybox through walls.
        if (!fogEnabled && dungeon)
        {
            constexpr float kDungeonFogDist = 75.0f;
            renderer.set_sky_settings(
                weatherFog.data(),
                kDungeonFogDist * 0.4f,
                kDungeonFogDist,
                false);
            return kDungeonFogDist;
        }

        // Atmospheric fog: starts gradually, reaches full opacity well before cull edge.
        auto fogStart = std::max(80.0f, viewDistance * kFogStartRatio);
        auto fogEnd = std::max(fogStart + 100.0f, viewDistance * kFogEndRatio);
        if (weatherMode == WeatherMode::Storm)
        {
            fogStart = std::max(40.0f, viewDistance * 0.22f);
            fogEnd = std::max(fogStart + 90.0f, viewDistance * 0.64f);
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            fogStart = std::max(28.0f, viewDistance * 0.16f);
            fogEnd = std::max(fogStart + 75.0f, viewDistance * 0.52f);
        }
        // Dungeons: override fog to a short black range regardless of slider.
        if (dungeon)
        {
            constexpr float kDungeonFogDist = 75.0f;
            renderer.set_sky_settings(
                weatherFog.data(),
                kDungeonFogDist * 0.4f,
                kDungeonFogDist,
                false);
            return kDungeonFogDist;
        }
        renderer.set_sky_settings(
            weatherFog.data(),
            fogStart,
            fogEnd,
            world.parsedSky && !world.skyFileName.empty());
        return fogEnd;
    }

    int nearest_available(int value, const std::vector<int>& values)
    {
        if (values.empty())
            return value;
        auto best = values.front();
        auto bestDistance = std::abs(best - value);
        for (const auto candidate : values)
        {
            const auto distance = std::abs(candidate - value);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::VulkanRenderer& renderer,
        bool& fogEnabled,
        bool& showSoundGizmos,
        bool& showMusicGizmos,
        bool& showPortalGizmos,
        bool& showEffectGizmos,
        bool& showNamePlates,
        bool& showCollisionDebug,
        bool& playMapSounds,
        bool& playMapMusic,
        float& masterVolume,
        int& selectedMapIndex,
        float& viewDistance,
        float& actorViewDistance,
        bool& actorsEnabled,
        WeatherMode& weatherMode,
        const std::vector<CharacterOption>& characterOptions,
        int& selectedCharacterOption,
        phoenix::character::CharacterAppearance& appearance,
        phoenix::character::CharacterSystem& characterSystem,
        phoenix::character::WeaponEffect& weaponEffect,
        bool& showEffectsWindow,
        bool& showActorAnimWindow,
        bool assetsReady,
        float camX, float camY, float camZ, float camYaw, float camPitch)
    {
        UnifiedPanelResult result{};

        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Phoenix Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return result;
        }

        // ---- World Map ----
        const auto& maps = runtime.world_map_names();
        if (!maps.empty())
        {
            selectedMapIndex = std::clamp(selectedMapIndex, 0, static_cast<int>(maps.size() - 1));
            ImGui::SetNextItemWidth(190.0f);
            if (ImGui::BeginCombo("##map", maps[static_cast<std::size_t>(selectedMapIndex)].c_str()))
            {
                for (std::size_t i = 0; i < maps.size(); ++i)
                {
                    if (ImGui::Selectable(maps[i].c_str(), selectedMapIndex == static_cast<int>(i)))
                        selectedMapIndex = static_cast<int>(i);
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            result.loadRequested = ImGui::Button("Load");
        }

        // ---- View settings ----
        const auto previousFog = fogEnabled;
        ImGui::Checkbox("Fog", &fogEnabled);
        if (fogEnabled != previousFog)
            apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);

        const auto previousViewDistance = viewDistance;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("View", &viewDistance, 100.0f, 2500.0f, "%.0f");
        result.viewDistanceChanged = std::abs(previousViewDistance - viewDistance) > 1.0f;

        const auto previousActorViewDistance = actorViewDistance;
        ImGui::SetNextItemWidth(200.0f);
        ImGui::SliderFloat("Actors", &actorViewDistance, 10.0f, 2500.0f, "%.0f");
        ImGui::SameLine();
        ImGui::Checkbox("##actorsOn", &actorsEnabled);
        result.viewDistanceChanged = result.viewDistanceChanged
            || std::abs(previousActorViewDistance - actorViewDistance) > 1.0f;

        const WeatherMode previousWeatherMode = weatherMode;
        const char* weatherItems[] = { "Default", "Storm", "Snowstorm", "Sunset", "Night" };
        int weatherIndex = static_cast<int>(weatherMode);
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Weather", &weatherIndex, weatherItems, IM_ARRAYSIZE(weatherItems)))
            weatherMode = static_cast<WeatherMode>(std::clamp(weatherIndex, 0, 4));
        result.weatherChanged = weatherMode != previousWeatherMode;

        // ---- Debug overlays ----
        if (ImGui::TreeNodeEx("Overlays", ImGuiTreeNodeFlags_DefaultOpen))
        {
            const bool prevSounds = showSoundGizmos;
            const bool prevMusic = showMusicGizmos;
            const bool prevPortals = showPortalGizmos;
            const bool prevEffects = showEffectGizmos;
            const bool prevCollision = showCollisionDebug;
            ImGui::Checkbox("Sounds", &showSoundGizmos);
            ImGui::SameLine();
            ImGui::Checkbox("Music", &showMusicGizmos);
            ImGui::Checkbox("Portals", &showPortalGizmos);
            ImGui::SameLine();
            ImGui::Checkbox("Effects", &showEffectGizmos);
            ImGui::Checkbox("Names", &showNamePlates);
            ImGui::SameLine();
            ImGui::Checkbox("Collision", &showCollisionDebug);
            ImGui::Checkbox("Play Sounds", &playMapSounds);
            ImGui::SameLine();
            ImGui::Checkbox("Play Music", &playMapMusic);
            ImGui::SetNextItemWidth(200.0f);
            ImGui::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f, "%.2f");
            ImGui::Separator();
            ImGui::TextDisabled("Debug windows");
            ImGui::Checkbox("Effects##win", &showEffectsWindow);
            ImGui::SameLine();
            ImGui::Checkbox("Actor anim##win", &showActorAnimWindow);
            result.debugGizmosChanged = prevSounds != showSoundGizmos
                || prevMusic != showMusicGizmos
                || prevPortals != showPortalGizmos
                || prevEffects != showEffectGizmos
                || prevCollision != showCollisionDebug;
            ImGui::TreePop();
        }

        // ---- Character ----
        if (!characterOptions.empty() && ImGui::TreeNodeEx("Character", ImGuiTreeNodeFlags_DefaultOpen))
        {
            if (!assetsReady)
            {
                ImGui::TextDisabled("Loading assets...");
                ImGui::TreePop();
            }
            else
            {
            // Snapshot current state to detect any change.
            const auto prevAppearance = appearance;
            const auto prevCharOption = selectedCharacterOption;

            selectedCharacterOption = std::clamp(selectedCharacterOption, 0, static_cast<int>(characterOptions.size() - 1));
            const auto& selected = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::BeginCombo("Model", selected.label.c_str()))
            {
                for (std::size_t i = 0; i < characterOptions.size(); ++i)
                {
                    const bool isSelected = selectedCharacterOption == static_cast<int>(i);
                    if (ImGui::Selectable(characterOptions[i].label.c_str(), isSelected))
                    {
                        selectedCharacterOption = static_cast<int>(i);
                        appearance.raceFolder = characterOptions[i].raceFolder;
                        appearance.prefix = characterOptions[i].prefix;
                        appearance.upperIndex = nearest_available(appearance.upperIndex, characterOptions[i].upperIndices);
                        appearance.lowerIndex = nearest_available(appearance.lowerIndex, characterOptions[i].lowerIndices);
                        appearance.handIndex = nearest_available(appearance.handIndex, characterOptions[i].handIndices);
                        appearance.footIndex = nearest_available(appearance.footIndex, characterOptions[i].footIndices);
                        appearance.helmetIndex = nearest_available(appearance.helmetIndex, characterOptions[i].helmetIndices);
                        appearance.faceIndex = nearest_available(appearance.faceIndex, characterOptions[i].faceIndices);
                        appearance.hairIndex = nearest_available(appearance.hairIndex, characterOptions[i].hairIndices);
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const auto& current = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
            appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
            appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
            appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
            appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
            appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
            appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
            appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
            ImGui::Checkbox("Helmet", &appearance.helmetVisible);
            if (appearance.helmetVisible)
            {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputInt("##helmet", &appearance.helmetIndex);
                appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
            }
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Upper", &appearance.upperIndex);
            appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Lower", &appearance.lowerIndex);
            appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Gloves", &appearance.handIndex);
            appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Boots", &appearance.footIndex);
            appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
            ImGui::SetNextItemWidth(80.0f);
            ImGui::InputInt("Face", &appearance.faceIndex);
            appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
            if (!appearance.helmetVisible)
            {
                ImGui::SetNextItemWidth(80.0f);
                ImGui::InputInt("Hair", &appearance.hairIndex);
                appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
            }

            // ---- Weapon / Shield ----
            ImGui::Separator();
            {
                using WT = phoenix::character::WeaponType;
                struct WeaponLabel { WT type; const char* label; };
                static constexpr WeaponLabel weaponLabels[] = {
                    { WT::None,       "None" },
                    { WT::Sword1H,    "Sword 1H" },
                    { WT::Sword2H,    "Sword 2H" },
                    { WT::Axe1H,      "Axe 1H" },
                    { WT::Axe2H,      "Axe 2H" },
                    { WT::DualSword,  "Dual Sword" },
                    { WT::Spear,      "Spear" },
                    { WT::Mace1H,     "Mace 1H" },
                    { WT::Hammer2H,   "Hammer 2H" },
                    { WT::RevDagger,  "Rev Dagger" },
                    { WT::Dagger,     "Dagger" },
                    { WT::Javelin,    "Javelin" },
                    { WT::Staff,      "Staff" },
                    { WT::Bow,        "Bow" },
                    { WT::Crossbow,   "Crossbow" },
                    { WT::Claw,       "Claw" },
                };
                static constexpr WeaponLabel shieldLabels[] = {
                    { WT::None,        "None" },
                    { WT::ShieldLight, "Shield (Light)" },
                    { WT::ShieldDark,  "Shield (Dark)" },
                };

                // Find current label for weapon combo.
                const char* currentWeaponLabel = "None";
                int currentWeaponIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                {
                    if (weaponLabels[i].type == appearance.weaponType)
                    {
                        currentWeaponLabel = weaponLabels[i].label;
                        currentWeaponIdx = i;
                        break;
                    }
                }

                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::BeginCombo("Weapon", currentWeaponLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    {
                        const bool isSelected = (i == currentWeaponIdx);
                        if (ImGui::Selectable(weaponLabels[i].label, isSelected))
                        {
                            appearance.weaponType = weaponLabels[i].type;
                            if (appearance.weaponType == WT::None)
                                appearance.weaponIndex = -1;
                            else if (appearance.weaponIndex < 0)
                                appearance.weaponIndex = 1;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (appearance.weaponType != WT::None)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##weapIdx", &appearance.weaponIndex);
                    if (appearance.weaponIndex < 1) appearance.weaponIndex = 1;
                }

                // Find current label for shield combo.
                const char* currentShieldLabel = "None";
                int currentShieldIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                {
                    if (shieldLabels[i].type == appearance.shieldType)
                    {
                        currentShieldLabel = shieldLabels[i].label;
                        currentShieldIdx = i;
                        break;
                    }
                }

                ImGui::SetNextItemWidth(130.0f);
                if (ImGui::BeginCombo("Shield", currentShieldLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    {
                        const bool isSelected = (i == currentShieldIdx);
                        if (ImGui::Selectable(shieldLabels[i].label, isSelected))
                        {
                            appearance.shieldType = shieldLabels[i].type;
                            if (appearance.shieldType == WT::None)
                                appearance.shieldIndex = -1;
                            else if (appearance.shieldIndex < 0)
                                appearance.shieldIndex = 1;
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (appearance.shieldType != WT::None)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##shldIdx", &appearance.shieldIndex);
                    if (appearance.shieldIndex < 1) appearance.shieldIndex = 1;
                }
            }

            // ---- Cloak ----
            ImGui::Separator();
            {
                bool hasCloak = appearance.cloakIndex > 0;
                if (ImGui::Checkbox("Cloak", &hasCloak))
                    appearance.cloakIndex = hasCloak ? 1 : -1;
                if (hasCloak)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##cloakIdx", &appearance.cloakIndex);
                    if (appearance.cloakIndex < 1) appearance.cloakIndex = 1;
                }
            }

            // ---- Mount (vehicle) ----
            ImGui::Separator();
            {
                ImGui::Checkbox("Mount", &appearance.mounted);
                if (appearance.mounted)
                {
                    static const char* mountClasses[] = { "hu", "de", "el", "vi" };
                    int classIdx = 0;
                    for (int i = 0; i < 4; ++i)
                        if (appearance.mountClass == mountClasses[i]) { classIdx = i; break; }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::BeginCombo("##mountClass", mountClasses[classIdx]))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const bool sel = (i == classIdx);
                            if (ImGui::Selectable(mountClasses[i], sel))
                                appearance.mountClass = mountClasses[i];
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##mountIdx", &appearance.mountIndex);
                    if (appearance.mountIndex < 0) appearance.mountIndex = 0;

                    const int maxMountBone = std::max(0, characterSystem.mount_bone_count() - 1);
                    if (maxMountBone > 0)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Seat bone", &characterSystem.mountBoneIndex);
                        characterSystem.mountBoneIndex = std::clamp(characterSystem.mountBoneIndex, 0, maxMountBone);
                    }
                }
            }

            // Bone attachment tuning for weapons/shields (live — no reload needed).
            {
                const int maxBone = std::max(0, characterSystem.animation_bone_count() - 1);
                bool showBoneSection = appearance.weaponType != phoenix::character::WeaponType::None
                    || appearance.shieldType != phoenix::character::WeaponType::None;
                if (showBoneSection && maxBone > 0)
                {
                    ImGui::Separator();
                    ImGui::Text("Bone attach (0-%d)", maxBone);
                    if (appearance.weaponType != phoenix::character::WeaponType::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Wpn bone", &characterSystem.weaponBoneIndex);
                        characterSystem.weaponBoneIndex = std::clamp(characterSystem.weaponBoneIndex, 0, maxBone);
                    }
                    if (appearance.shieldType != phoenix::character::WeaponType::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Shld bone", &characterSystem.shieldBoneIndex);
                        characterSystem.shieldBoneIndex = std::clamp(characterSystem.shieldBoneIndex, 0, maxBone);
                    }
                }
            }

            // Detect any change — triggers instant reload (no Apply button needed).
            result.characterChanged = selectedCharacterOption != prevCharOption
                || appearance.raceFolder != prevAppearance.raceFolder
                || appearance.prefix != prevAppearance.prefix
                || appearance.upperIndex != prevAppearance.upperIndex
                || appearance.lowerIndex != prevAppearance.lowerIndex
                || appearance.handIndex != prevAppearance.handIndex
                || appearance.footIndex != prevAppearance.footIndex
                || appearance.helmetIndex != prevAppearance.helmetIndex
                || appearance.faceIndex != prevAppearance.faceIndex
                || appearance.hairIndex != prevAppearance.hairIndex
                || appearance.helmetVisible != prevAppearance.helmetVisible
                || appearance.weaponType != prevAppearance.weaponType
                || appearance.weaponIndex != prevAppearance.weaponIndex
                || appearance.shieldType != prevAppearance.shieldType
                || appearance.shieldIndex != prevAppearance.shieldIndex
                || appearance.cloakIndex != prevAppearance.cloakIndex
                || appearance.mounted != prevAppearance.mounted
                || appearance.mountClass != prevAppearance.mountClass
                || appearance.mountIndex != prevAppearance.mountIndex;
            ImGui::TreePop();
            } // else (assetsReady)
        }

        // ---- Emotes ----
        if (ImGui::TreeNode("Emotes"))
        {
            for (int i = 1; i <= 10; ++i)
            {
                char label[16];
                std::snprintf(label, sizeof(label), "Anim %d", i);
                if (ImGui::Button(label, ImVec2(60.0f, 0.0f)))
                    result.emoteTriggered = i;
                if (i % 5 != 0) ImGui::SameLine();
            }
            ImGui::TreePop();
        }

        // ---- Weapon aura (procedural particles, layered) ----
        ImGui::Separator();
        if (ImGui::TreeNode("Weapon aura"))
        {
            using WE = phoenix::character::WeaponEffect;
            ImGui::Checkbox("Enabled", &weaponEffect.enabled());

            // Preset → layer applier (combine elements by stacking layers).
            static int presetIdx = 0;
            static int targetLayer = 0;
            const char* presetNames[WE::kPresetCount] = {
                "Fire", "Ice", "Holy", "Poison", "Shadow", "Arcane" };
            ImGui::SetNextItemWidth(110.0f);
            ImGui::Combo("Preset", &presetIdx, presetNames, WE::kPresetCount);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(50.0f);
            ImGui::Combo("##presetLayer", &targetLayer, "L0\0L1\0L2\0");
            ImGui::SameLine();
            if (ImGui::Button("Apply"))
                weaponEffect.apply_preset(targetLayer, static_cast<WE::Preset>(presetIdx));

            static const char* axisNames[] = { "X", "Y", "Z" };
            for (int i = 0; i < WE::kMaxLayers; ++i)
            {
                ImGui::PushID(i);
                auto& L = weaponEffect.layer(i);
                char hdr[24];
                std::snprintf(hdr, sizeof(hdr), "Layer %d%s", i, L.enabled ? " (on)" : "");
                if (ImGui::TreeNode(hdr))
                {
                    ImGui::Checkbox("On", &L.enabled);
                    ImGui::ColorEdit3("Birth", L.colorStart);
                    ImGui::ColorEdit3("Death", L.colorEnd);
                    ImGui::SliderFloat("Intensity", &L.intensity, 0.0f, 3.0f, "%.2f");
                    ImGui::SliderFloat("Spawn/s", &L.spawnRate, 0.0f, 400.0f, "%.0f");
                    ImGui::SliderFloat("Flow speed", &L.flowSpeed, -2.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat("Lifetime", &L.lifetime, 0.1f, 3.0f, "%.2f");
                    ImGui::SliderFloat("Size", &L.size, 0.01f, 0.25f, "%.3f");
                    ImGui::SliderFloat("Blade length", &L.bladeLength, 0.0f, 2.0f, "%.2f");
                    ImGui::SliderFloat("Swirl radius", &L.radius, 0.0f, 0.4f, "%.3f");
                    ImGui::SliderFloat("Swirl speed", &L.swirl, -8.0f, 8.0f, "%.2f");
                    L.axis = std::clamp(L.axis, 0, 2);
                    ImGui::SetNextItemWidth(60.0f);
                    if (ImGui::BeginCombo("Blade axis", axisNames[L.axis]))
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            const bool sel = (a == L.axis);
                            if (ImGui::Selectable(axisNames[a], sel)) L.axis = a;
                            if (sel) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (!characterSystem.weapon_attachment().valid)
                ImGui::TextDisabled("Equip a weapon to anchor the aura.");
            ImGui::TreePop();
        }

        // ---- Coordinates ----
        ImGui::Separator();
        ImGui::Text("X: %.1f  Y: %.1f  Z: %.1f", camX, camY, camZ);
        ImGui::Text("Yaw: %.2f  Pitch: %.2f", camYaw, camPitch);

        ImGui::End();
        return result;
    }
}
