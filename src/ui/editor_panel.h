#pragma once

#include "character/character_system.h"
#include "character/weapon_effect.h"
#include "effects/effect_system.h"
#include "renderer/vulkan_renderer.h"
#include "runtime/phoenix_runtime.h"

#include <string>
#include <vector>

namespace phoenix::ui
{
    enum class WeatherMode
    {
        Default,
        Dawn,
        MidAfternoon,
        Dusk,
        Sunset,
        Night,
        Overcast,
        Storm,
        Snowstorm,
    };

    enum class WaterMode
    {
        Natural,
        Ocean,
        Tropical,
        River,
        Lake,
        Cold,
        Swamp,
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
        bool waterChanged{};
        int emoteTriggered{};   // 0=none, 1-10=emote number (one-shot)
        int botSpawnCount{};
        bool clearBots{};
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

    void apply_renderer_water_style(
        phoenix::renderer::VulkanRenderer& renderer,
        WaterMode waterMode);

    // The main ImGui control panel (world map, weather, character, weapon aura).
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
        float cameraYaw);
}
