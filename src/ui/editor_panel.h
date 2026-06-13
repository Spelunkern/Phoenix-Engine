#pragma once

#include "character/character_system.h"
#include "character/monster_manager.h"
#include "character/npc_manager.h"
#include "character/weapon_effect.h"
#include "effects/effect_system.h"
#include "renderer/vulkan_renderer.h"
#include "runtime/phoenix_runtime.h"

#include <filesystem>
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
        std::size_t animationTriggered{}; // 0=none, otherwise direct animation index (one-shot)
        int botSpawnCount{};
        bool clearBots{};
        int npcSpawnCatalogIndex{ -1 };
        int npcSpawnCount{ 1 };
        bool clearNpcs{};
        int monsterSpawnCatalogIndex{ -1 };
        int monsterSpawnCount{ 1 };
        bool clearMonsters{};
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

    // Display options persisted to display.ini next to the executable
    // (same pattern as perf_hud.ini): loaded at startup, saved on exit.
    struct DisplaySettings
    {
        bool characterShadow{ true };
    };
    DisplaySettings load_display_settings(const std::filesystem::path& executableDir);
    void save_display_settings(const std::filesystem::path& executableDir, const DisplaySettings& settings);

    // The main ImGui control panel (world map, weather, character, weapon aura).
    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::VulkanRenderer& renderer,
        bool& fogEnabled,
        bool& showCollisionDebug,
        bool& showCharacterShadow,
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
        float& botViewDistance,
        const std::vector<phoenix::character::NpcCatalogEntry>& npcCatalog,
        std::size_t npcActiveCount,
        const std::string& npcStatus,
        float& npcViewDistance,
        const std::vector<phoenix::character::MonsterCatalogEntry>& monsterCatalog,
        std::size_t monsterActiveCount,
        const std::string& monsterStatus,
        float& monsterViewDistance,
        bool assetsReady,
        float cameraX,
        float cameraY,
        float cameraZ,
        float cameraYaw);
}
