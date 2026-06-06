#include "ui/editor_panel.h"
#include "ui/cpu_profiler.h"

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
        else if (weatherMode == WeatherMode::Dawn)
        {
            weatherFog = { 0.62f, 0.48f, 0.42f };
            skyTuning[3] = 5.0f;
        }
        else if (weatherMode == WeatherMode::Dusk)
        {
            weatherFog = { 0.38f, 0.22f, 0.32f };
            skyTuning[3] = 6.0f;
        }
        else if (weatherMode == WeatherMode::MidAfternoon)
        {
            weatherFog = { 0.82f, 0.72f, 0.52f };
            skyTuning[3] = 7.0f;
        }
        else if (weatherMode == WeatherMode::Overcast)
        {
            weatherFog = { 0.52f, 0.54f, 0.56f };
            skyTuning[3] = 8.0f;
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
            renderer.set_sky_settings(
                weatherFog.data(),
                viewDistance * 0.7f,
                viewDistance,
                false);
            return viewDistance;
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
        else if (weatherMode == WeatherMode::Overcast)
        {
            fogStart = std::max(60.0f, viewDistance * 0.28f);
            fogEnd = std::max(fogStart + 100.0f, viewDistance * 0.72f);
        }
        if (dungeon)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                viewDistance * 0.7f,
                viewDistance,
                false);
            return viewDistance;
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

    void apply_renderer_water_style(
        phoenix::renderer::VulkanRenderer& renderer,
        WaterMode waterMode)
    {
        const float* rgba = nullptr;
        static constexpr float natural[4]{ 0.10f, 0.18f, 0.22f, 0.62f };
        static constexpr float ocean[4]{ 0.02f, 0.20f, 0.42f, 0.66f };
        static constexpr float tropical[4]{ 0.02f, 0.48f, 0.62f, 0.54f };
        static constexpr float river[4]{ 0.07f, 0.25f, 0.30f, 0.58f };
        static constexpr float lake[4]{ 0.03f, 0.24f, 0.36f, 0.60f };
        static constexpr float cold[4]{ 0.14f, 0.34f, 0.46f, 0.54f };
        static constexpr float swamp[4]{ 0.12f, 0.24f, 0.14f, 0.62f };

        switch (waterMode)
        {
        case WaterMode::Ocean: rgba = ocean; break;
        case WaterMode::Tropical: rgba = tropical; break;
        case WaterMode::River: rgba = river; break;
        case WaterMode::Lake: rgba = lake; break;
        case WaterMode::Cold: rgba = cold; break;
        case WaterMode::Swamp: rgba = swamp; break;
        case WaterMode::Natural:
        default: rgba = natural; break;
        }
        renderer.set_water_style(rgba);
    }

    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::VulkanRenderer& renderer,
        bool& fogEnabled,
        bool& showCollisionDebug,
        bool& playMapSounds,
        bool& playMapMusic,
        float& masterVolume,
        int& selectedMapIndex,
        float& viewDistance,
        WeatherMode& weatherMode,
        WaterMode& waterMode,
        const std::vector<CharacterOption>& characterOptions,
        int& selectedCharacterOption,
        phoenix::character::CharacterAppearance& appearance,
        phoenix::character::CharacterSystem& characterSystem,
        phoenix::character::WeaponEffect& weaponEffect,
        phoenix::effects::EffectManager& effectManager,
        bool botControlsAvailable,
        std::size_t botCount,
        bool& botEffectsEnabled,
        bool& botWeaponAurasEnabled,
        bool assetsReady,
        float cameraX,
        float cameraY,
        float cameraZ,
        float cameraYaw)
    {
        UnifiedPanelResult result{};
        const auto prevAppearance = appearance;
        const auto prevCharOption = selectedCharacterOption;

        enum class Section : int
        {
            Map,
            Display,
            Sound,
            Effects,
            Character,
            Vehicle,
            Bots,
            Emotes,
        };
        static Section activeSection = Section::Map;

        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Phoenix Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return result;
        }

        auto sectionButton = [&](Section section, const char* label) {
            const bool selected = activeSection == section;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label, ImVec2(68.0f, 0.0f)))
                activeSection = section;
            if (selected)
                ImGui::PopStyleColor();
        };

        sectionButton(Section::Map, "Map##nav"); ImGui::SameLine();
        sectionButton(Section::Display, "Display##nav"); ImGui::SameLine();
        sectionButton(Section::Sound, "Sound##nav"); ImGui::SameLine();
        sectionButton(Section::Effects, "Effects##nav");
        sectionButton(Section::Character, "Character##nav"); ImGui::SameLine();
        sectionButton(Section::Vehicle, "Vehicle##nav"); ImGui::SameLine();
        sectionButton(Section::Bots, "Bots##nav"); ImGui::SameLine();
        sectionButton(Section::Emotes, "Emotes##nav");
        ImGui::Separator();

        if (activeSection == Section::Map)
        {
            const auto& maps = runtime.world_map_names();
            if (!maps.empty())
            {
                selectedMapIndex = std::clamp(selectedMapIndex, 0, static_cast<int>(maps.size() - 1));
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::BeginCombo("Map##mapCombo", maps[static_cast<std::size_t>(selectedMapIndex)].c_str()))
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

            const auto previousFog = fogEnabled;
            ImGui::Checkbox("Fog", &fogEnabled);
            if (fogEnabled != previousFog)
                apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);

            const auto previousViewDistance = viewDistance;
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat("Fog distance", &viewDistance, 100.0f, 2500.0f, "%.0f");
            result.viewDistanceChanged = std::abs(previousViewDistance - viewDistance) > 1.0f;

            const WeatherMode previousWeatherMode = weatherMode;
            const char* weatherItems[] = { "Default", "Dawn", "Mid-Afternoon", "Dusk", "Sunset", "Night", "Overcast", "Storm", "Snowstorm" };
            int weatherIndex = static_cast<int>(weatherMode);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Sky", &weatherIndex, weatherItems, IM_ARRAYSIZE(weatherItems)))
                weatherMode = static_cast<WeatherMode>(std::clamp(weatherIndex, 0, 8));
            result.weatherChanged = weatherMode != previousWeatherMode;

            const WaterMode previousWaterMode = waterMode;
            const char* waterItems[] = { "Natural", "Ocean", "Tropical", "River", "Lake", "Cold", "Swamp" };
            int waterIndex = static_cast<int>(waterMode);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Water", &waterIndex, waterItems, IM_ARRAYSIZE(waterItems)))
            {
                waterMode = static_cast<WaterMode>(std::clamp(waterIndex, 0, 6));
                apply_renderer_water_style(renderer, waterMode);
            }
            result.waterChanged = waterMode != previousWaterMode;
        }
        else if (activeSection == Section::Display)
        {
            const bool prevCollision = showCollisionDebug;
            ImGui::Checkbox("Collision", &showCollisionDebug);
            result.debugGizmosChanged = prevCollision != showCollisionDebug;
        }
        else if (activeSection == Section::Sound)
        {
            ImGui::Checkbox("Play Sounds", &playMapSounds);
            ImGui::Checkbox("Play Music", &playMapMusic);
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f, "%.2f");
        }
        else if (activeSection == Section::Effects)
        {
            ImGui::TextDisabled("Effects spawner");
            {
                using namespace phoenix::effects;
                const auto& catalog = preset_catalog();
                const int categoryCount = static_cast<int>(EffectCategory::Count);

                static int catFilter = 0;
                static int prevCatFilter = 0;
                const char* curCat = (catFilter == 0)
                    ? "All"
                    : category_name(static_cast<EffectCategory>(catFilter - 1));
                ImGui::SetNextItemWidth(120.0f);
                if (ImGui::BeginCombo("Category##effectSpawner", curCat))
                {
                    if (ImGui::Selectable("All", catFilter == 0))
                        catFilter = 0;
                    for (int ci = 0; ci < categoryCount; ++ci)
                    {
                        const bool sel = catFilter == ci + 1;
                        if (ImGui::Selectable(category_name(static_cast<EffectCategory>(ci)), sel))
                            catFilter = ci + 1;
                        if (sel)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                std::vector<int> filtered;
                filtered.reserve(catalog.size());
                for (int i = 0; i < static_cast<int>(catalog.size()); ++i)
                    if (catFilter == 0 || static_cast<int>(catalog[static_cast<std::size_t>(i)].category) == catFilter - 1)
                        filtered.push_back(i);

                static int selInFiltered = 0;
                if (catFilter != prevCatFilter)
                {
                    selInFiltered = 0;
                    prevCatFilter = catFilter;
                }

                float sx = cameraX;
                float sy = cameraY;
                float sz = cameraZ;
                if (botControlsAvailable && characterSystem.ready())
                {
                    sx = characterSystem.world_x();
                    sy = characterSystem.world_y();
                    sz = characterSystem.world_z();
                }

                if (filtered.empty())
                {
                    ImGui::TextDisabled("(no effects in category)");
                }
                else
                {
                    selInFiltered = std::clamp(selInFiltered, 0, static_cast<int>(filtered.size()) - 1);
                    const auto curName = catalog[static_cast<std::size_t>(filtered[static_cast<std::size_t>(selInFiltered)])].name.c_str();
                    ImGui::SetNextItemWidth(200.0f);
                    if (ImGui::BeginCombo("Effect##effectSpawner", curName))
                    {
                        for (int k = 0; k < static_cast<int>(filtered.size()); ++k)
                        {
                            const bool sel = k == selInFiltered;
                            const auto name = catalog[static_cast<std::size_t>(filtered[static_cast<std::size_t>(k)])].name.c_str();
                            if (ImGui::Selectable(name, sel))
                                selInFiltered = k;
                            if (sel)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }

                    const auto& def = catalog[static_cast<std::size_t>(filtered[static_cast<std::size_t>(selInFiltered)])];
                    const float fx = std::sin(cameraYaw);
                    const float fz = std::cos(cameraYaw);
                    if (def.projectile)
                    {
                        if (ImGui::Button("Cast (forward)##effectSpawner"))
                        {
                            const float vel[3] = { fx * def.projectileSpeed, 0.0f, fz * def.projectileSpeed };
                            const float travel = def.projectileRange / std::max(0.1f, def.projectileSpeed);
                            effectManager.spawn(def,
                                EffectAnchor::at(sx + fx * 0.6f, sy + 1.0f, sz + fz * 0.6f), vel, travel);
                        }
                    }
                    else
                    {
                        if (ImGui::Button("Spawn at character##effectSpawner"))
                            effectManager.spawn(def, EffectAnchor::at(sx, sy, sz));
                        ImGui::SameLine();
                        if (ImGui::Button("Spawn ahead##effectSpawner"))
                            effectManager.spawn(def, EffectAnchor::at(sx + fx * 4.0f, sy, sz + fz * 4.0f));
                    }
                }

                if (ImGui::Button("Clear all effects##effectSpawner"))
                    effectManager.clear();
                ImGui::Text("Active: %zu  |  Library: %zu", effectManager.active_count(), catalog.size());
                ImGui::TextDisabled("G: impact at weapon/character");
            }
            ImGui::Separator();
            ImGui::TextDisabled("Weapon aura");
            using WE = phoenix::character::WeaponEffect;
            ImGui::Checkbox("Enabled", &weaponEffect.enabled());

            static int presetIdx = 0;
            static int targetLayer = 0;
            const char* presetNames[WE::kPresetCount] = {
                "Fire", "Ice", "Holy", "Poison", "Shadow", "Arcane" };
            ImGui::SetNextItemWidth(120.0f);
            ImGui::Combo("Preset", &presetIdx, presetNames, WE::kPresetCount);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(60.0f);
            ImGui::Combo("Layer", &targetLayer, "L0\0L1\0L2\0");
            ImGui::SameLine();
            if (ImGui::Button("Apply"))
                weaponEffect.apply_preset(targetLayer, static_cast<WE::Preset>(presetIdx));

            static const char* axisNames[] = { "X", "Y", "Z" };
            for (int i = 0; i < WE::kMaxLayers; ++i)
            {
                ImGui::PushID(i);
                auto& layer = weaponEffect.layer(i);
                ImGui::Separator();
                char label[32];
                std::snprintf(label, sizeof(label), "Layer %d##weaponAuraLayer", i);
                if (ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_None))
                {
                    ImGui::Checkbox("Enabled", &layer.enabled);
                    ImGui::ColorEdit3("Birth", layer.colorStart);
                    ImGui::ColorEdit3("Death", layer.colorEnd);
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Intensity", &layer.intensity, 0.0f, 3.0f, "%.2f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Spawn/s", &layer.spawnRate, 0.0f, 400.0f, "%.0f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Flow", &layer.flowSpeed, -2.0f, 2.0f, "%.2f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Lifetime", &layer.lifetime, 0.1f, 3.0f, "%.2f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Size", &layer.size, 0.01f, 0.25f, "%.3f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Blade length", &layer.bladeLength, 0.0f, 2.0f, "%.2f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Swirl radius", &layer.radius, 0.0f, 0.4f, "%.3f");
                    ImGui::SetNextItemWidth(210.0f);
                    ImGui::SliderFloat("Swirl speed", &layer.swirl, -8.0f, 8.0f, "%.2f");
                    layer.axis = std::clamp(layer.axis, 0, 2);
                    ImGui::SetNextItemWidth(70.0f);
                    if (ImGui::BeginCombo("Blade axis", axisNames[layer.axis]))
                    {
                        for (int a = 0; a < 3; ++a)
                        {
                            const bool selected = a == layer.axis;
                            if (ImGui::Selectable(axisNames[a], selected)) layer.axis = a;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
            if (!characterSystem.weapon_attachment().valid)
                ImGui::TextDisabled("Equip a weapon to anchor the aura.");
        }
        else if (activeSection == Section::Character)
        {
            if (characterOptions.empty())
            {
                ImGui::TextDisabled("No character models found.");
            }
            else if (!assetsReady)
            {
                ImGui::TextDisabled("Loading assets...");
            }
            else
            {
                selectedCharacterOption = std::clamp(selectedCharacterOption, 0, static_cast<int>(characterOptions.size() - 1));
                const auto& selected = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
                ImGui::SetNextItemWidth(190.0f);
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
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Upper", &appearance.upperIndex);
                appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Lower", &appearance.lowerIndex);
                appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Gloves", &appearance.handIndex);
                appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Boots", &appearance.footIndex);
                appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Face", &appearance.faceIndex);
                appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
                if (!appearance.helmetVisible)
                {
                    ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Hair", &appearance.hairIndex);
                    appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
                }

                ImGui::Separator();
                using WT = phoenix::character::WeaponType;
                struct WeaponLabel { WT type; const char* label; };
                static constexpr WeaponLabel weaponLabels[] = {
                    { WT::None, "None" }, { WT::Sword1H, "Sword 1H" }, { WT::Sword2H, "Sword 2H" },
                    { WT::Axe1H, "Axe 1H" }, { WT::Axe2H, "Axe 2H" }, { WT::DualSword, "Dual Sword" },
                    { WT::Spear, "Spear" }, { WT::Mace1H, "Mace 1H" }, { WT::Hammer2H, "Hammer 2H" },
                    { WT::RevDagger, "Rev Dagger" }, { WT::Dagger, "Dagger" }, { WT::Javelin, "Javelin" },
                    { WT::Staff, "Staff" }, { WT::Bow, "Bow" }, { WT::Crossbow, "Crossbow" }, { WT::Claw, "Claw" },
                };
                static constexpr WeaponLabel shieldLabels[] = {
                    { WT::None, "None" }, { WT::ShieldLight, "Shield (Light)" }, { WT::ShieldDark, "Shield (Dark)" },
                };

                const char* currentWeaponLabel = "None";
                int currentWeaponIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    if (weaponLabels[i].type == appearance.weaponType) { currentWeaponLabel = weaponLabels[i].label; currentWeaponIdx = i; break; }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Weapon", currentWeaponLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    {
                        const bool isSelected = i == currentWeaponIdx;
                        if (ImGui::Selectable(weaponLabels[i].label, isSelected))
                        {
                            appearance.weaponType = weaponLabels[i].type;
                            if (appearance.weaponType == WT::None) appearance.weaponIndex = -1;
                            else if (appearance.weaponIndex < 0) appearance.weaponIndex = 1;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
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

                const char* currentShieldLabel = "None";
                int currentShieldIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    if (shieldLabels[i].type == appearance.shieldType) { currentShieldLabel = shieldLabels[i].label; currentShieldIdx = i; break; }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Shield", currentShieldLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    {
                        const bool isSelected = i == currentShieldIdx;
                        if (ImGui::Selectable(shieldLabels[i].label, isSelected))
                        {
                            appearance.shieldType = shieldLabels[i].type;
                            if (appearance.shieldType == WT::None) appearance.shieldIndex = -1;
                            else if (appearance.shieldIndex < 0) appearance.shieldIndex = 1;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
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

                ImGui::Separator();
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

                const int maxBone = std::max(0, characterSystem.animation_bone_count() - 1);
                const bool showBoneSection = appearance.weaponType != WT::None || appearance.shieldType != WT::None;
                if (showBoneSection && maxBone > 0)
                {
                    ImGui::Separator();
                    ImGui::Text("Bone attach (0-%d)", maxBone);
                    if (appearance.weaponType != WT::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Wpn bone", &characterSystem.weaponBoneIndex);
                        characterSystem.weaponBoneIndex = std::clamp(characterSystem.weaponBoneIndex, 0, maxBone);
                    }
                    if (appearance.shieldType != WT::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Shld bone", &characterSystem.shieldBoneIndex);
                        characterSystem.shieldBoneIndex = std::clamp(characterSystem.shieldBoneIndex, 0, maxBone);
                    }
                }
            }
        }
        else if (activeSection == Section::Vehicle)
        {
            if (!assetsReady)
            {
                ImGui::TextDisabled("Loading assets...");
            }
            else
            {
                ImGui::Checkbox("Mount", &appearance.mounted);
                if (appearance.mounted)
                {
                    static const char* mountClasses[] = { "hu", "de", "el", "vi" };
                    int classIdx = 0;
                    for (int i = 0; i < 4; ++i)
                        if (appearance.mountClass == mountClasses[i]) { classIdx = i; break; }
                    ImGui::SetNextItemWidth(80.0f);
                    if (ImGui::BeginCombo("Class", mountClasses[classIdx]))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const bool selected = i == classIdx;
                            if (ImGui::Selectable(mountClasses[i], selected))
                                appearance.mountClass = mountClasses[i];
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("Index", &appearance.mountIndex);
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
        }
        else if (activeSection == Section::Bots)
        {
            ImGui::Text("Bots: %d", static_cast<int>(botCount));
            if (botControlsAvailable)
            {
                if (ImGui::Button("Spawn 10", ImVec2(95.0f, 0.0f)))
                    result.botSpawnCount = 10;
                ImGui::SameLine();
                if (ImGui::Button("Spawn 100", ImVec2(95.0f, 0.0f)))
                    result.botSpawnCount = 100;
                if (ImGui::Button("Clear All", ImVec2(195.0f, 0.0f)))
                    result.clearBots = true;
                ImGui::Checkbox("Bot Effects", &botEffectsEnabled);
                ImGui::Checkbox("Weapon Auras", &botWeaponAurasEnabled);
            }
            else
            {
                ImGui::TextDisabled("Playable character required.");
            }
        }
        else if (activeSection == Section::Emotes)
        {
            for (int i = 1; i <= 10; ++i)
            {
                char label[16];
                std::snprintf(label, sizeof(label), "Anim %d", i);
                if (ImGui::Button(label, ImVec2(60.0f, 0.0f)))
                    result.emoteTriggered = i;
                if (i % 5 != 0) ImGui::SameLine();
            }
        }

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

        ImGui::End();
        return result;
    }
}
