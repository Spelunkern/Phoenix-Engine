#pragma once

#include "character/character_system.h"
#include "character/monster_manager.h"
#include "character/npc_manager.h"
#include "renderer/opengl_renderer.h"
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
        bool characterChanged{};
        bool weatherChanged{};
        int emoteTriggered{};   // 0=none, 1-10=emote number (one-shot)
        std::size_t animationTriggered{}; // 0=none, otherwise direct animation index (one-shot)
        int botSpawnCount{};
        bool clearBots{};
        int npcSpawnCatalogIndex{ -1 };
        int npcSpawnCount{ 1 };
        bool npcSpawnRandom{};   // spawn npcSpawnCount of random catalog entries
        bool clearNpcs{};
        int monsterSpawnCatalogIndex{ -1 };
        int monsterSpawnCount{ 1 };
        bool monsterSpawnRandom{};
        bool clearMonsters{};
        bool effectSpawnRequested{};
        bool effectSpawnOneShot{};
        bool clearEffects{};
        // Diagnostic isolation: spawn one raw component (library.effects
        // index) directly, skipping sequence expansion.
        bool effectComponentSpawnRequested{};
    };

    // Nearest existing index in a sorted/unsorted list (keeps part selections valid
    // when switching character presets).
    int nearest_available(int value, const std::vector<int>& values);

    // Push fog / sky tuning to the renderer for the current weather mode.
    // Returns the fog-end distance (the cull boundary — nothing beyond this is
    // visible). When fog is disabled, returns a very large distance.
    float apply_renderer_fog(
        phoenix::renderer::OpenGLRenderer& renderer,
        const phoenix::runtime::PhoenixRuntime& runtime,
        bool fogEnabled,
        float viewDistance,
        WeatherMode weatherMode);

    // Display options persisted via app_settings.h (a custom section inside
    // the shared imgui.ini).
    struct DisplaySettings
    {
        bool characterShadow{ true };
    };

    // The main ImGui control panel (world map, weather, character).
    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::OpenGLRenderer& renderer,
        bool& fogEnabled,
        bool& showCharacterShadow,
        bool& playMapSounds,
        bool& playMapMusic,
        float& masterVolume,
        int& selectedMapIndex,
        float& viewDistance,
        WeatherMode& weatherMode,
        const std::vector<CharacterOption>& characterOptions,
        int& selectedCharacterOption,
        phoenix::character::CharacterAppearance& appearance,
        phoenix::character::CharacterSystem& characterSystem,
        bool botControlsAvailable,
        std::size_t botCount,
        float& botViewDistance,
        bool& botEffectsEnabled,
        const std::vector<phoenix::character::NpcCatalogEntry>& npcCatalog,
        std::size_t npcActiveCount,
        const std::string& npcStatus,
        float& npcViewDistance,
        const std::vector<phoenix::character::MonsterCatalogEntry>& monsterCatalog,
        std::size_t monsterActiveCount,
        const std::string& monsterStatus,
        float& monsterViewDistance,
        // Debug "Effects" panel: one entry per effect_library().sequences[i],
        // typically "<i>: <raw sequence name>" (names may be garbled — the
        // .EFT format stores them in a Korean codepage this UI doesn't
        // transcode — but the index is always usable for lookup).
        // Debug "Effects" panel: full data/effects catalog (effectFileNames,
        // from PhoenixRuntime::effect_library_files()), then the sequences of
        // whichever file is currently selected (rebuilt by the caller when
        // selectedEffectFileIndex changes). Names may be garbled — the .EFT
        // format stores them in a Korean codepage this UI doesn't transcode —
        // but the index is always usable for lookup.
        const std::vector<std::string>& effectFileNames,
        int& selectedEffectFileIndex,
        const std::vector<std::string>& effectSequenceNames,
        int& selectedEffectSequenceIndex,
        std::size_t effectActiveCount,
        float& effectSpawnYOffset,
        // Diagnostic isolation: raw component index (library.effects) to
        // spawn directly, bypassing sequence expansion.
        int& effectComponentIndex,
        bool assetsReady);
}
