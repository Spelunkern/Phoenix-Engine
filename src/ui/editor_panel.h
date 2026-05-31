#pragma once

#include "character/character_system.h"
#include "character/weapon_effect.h"
#include "renderer/vulkan_renderer.h"
#include "runtime/phoenix_runtime.h"

#include <string>
#include <vector>

namespace phoenix::ui
{
    enum class WeatherMode
    {
        Default,
        Storm,
        Snowstorm,
        Sunset,
        Night,
    };

    // One selectable character preset discovered by scanning the data folder.
    struct CharacterOption
    {
        std::string raceFolder;
        std::string prefix;
        std::string label;
        std::vector<int> upperIndices;
        std::vector<int> lowerIndices;
        std::vector<int> handIndices;
        std::vector<int> footIndices;
        std::vector<int> helmetIndices;
        std::vector<int> faceIndices;
        std::vector<int> hairIndices;
    };

    // Flags returned by the editor panel so the main loop can react (reload map,
    // reapply fog, rebuild gizmos, reload the character, etc.).
    struct UnifiedPanelResult
    {
        bool loadRequested{};
        bool viewDistanceChanged{};
        bool debugGizmosChanged{};
        bool characterChanged{};
        bool weatherChanged{};
    };

    // Nearest existing index in a sorted/unsorted list (keeps part selections valid
    // when switching character presets).
    int nearest_available(int value, const std::vector<int>& values);

    // Push fog / sky tuning to the renderer for the current weather mode.
    // Returns the fog-end distance (the cull boundary — nothing beyond this is
    // visible). When fog is disabled, returns a very large distance.
    float apply_renderer_fog(
        phoenix::renderer::VulkanRenderer& renderer,
        const phoenix::runtime::PhoenixRuntime& runtime,
        bool fogEnabled,
        float viewDistance,
        WeatherMode weatherMode);

    // The main ImGui control panel (world map, weather, character, weapon aura).
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
        float camX, float camY, float camZ, float camYaw, float camPitch);
}
