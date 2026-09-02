#include "app/bootstrap.h"
#include "app/loading_scheduler.h"
#include "audio/map_audio_scene.h"
#include "app/renderer_uploads.h"
#include "app/map_session.h"
#include "app/water_resources.h"
#include "assets/data_index.h"
#include "audio/audio_system.h"
#include "character/character_system.h"
#include "character/bot_manager.h"
#include "character/character_options.h"
#include "character/monster_manager.h"
#include "character/npc_manager.h"
#include "runtime/playable_spawn.h"
#include "runtime/effect_particle_system.h"
#include "runtime/weather_particle_system.h"
#include "runtime/celestial_system.h"
#include "runtime/star_field.h"
#include "runtime/phoenix_runtime.h"
#include "runtime/world_streamer.h"
#include "platform/sdl_window.h"
#include "renderer/dds_loader.h"
#include "renderer/visibility_culling.h"
#include "renderer/opengl_renderer.h"
#include "ui/app_settings.h"
#include "ui/editor_panel.h"
#include "ui/loading_screen.h"
#include "ui/perf_hud.h"
#include "ui/phoenix_ui.h"
#include "ui/weather_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // A file loaded by the debug "Effects" panel, with its textures resolved
    // to layer indices in OpenGLRenderer's dedicated debug-effect texture
    // array (top bit set — see effect_particle.frag) rather than the shared
    // terrain/asset array the map's own effect file uses.
    struct DebugEffectLibraryEntry
    {
        phoenix::world::EftLibrary library;
        std::vector<std::uint32_t> textureLayers; // parallel to library.textureNames
        std::vector<phoenix::world::EftMesh> meshes;
    };

    struct StreamedWorldScene
    {
        phoenix::runtime::StaticObjectScene staticObjects;
        phoenix::runtime::AnimatedObjectScene animatedObjects;
        phoenix::runtime::WorldCollisionMesh collision;
        std::vector<phoenix::runtime::PhoenixRuntime::LadderVolume> ladders;
    };

    struct StreamedTerrainScene
    {
        std::vector<phoenix::renderer::TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        phoenix::runtime::PhoenixRuntime::TerrainLodInfo lod;
        float centerX{};
        float centerZ{};
        float residentDistance{};
    };

    // UI types/functions live in ui/editor_panel.{h,cpp} and ui/perf_hud.{h,cpp}.
    using phoenix::ui::WeatherMode;
    using phoenix::ui::CharacterOption;
    using phoenix::ui::UnifiedPanelResult;
    using phoenix::ui::apply_renderer_fog;
    using phoenix::ui::draw_editor_panel;
    using phoenix::ui::PerfHudState;
    using phoenix::ui::draw_perf_hud;
    using phoenix::ui::draw_weather_overlay;
    using phoenix::ui::make_loading_image;
    using phoenix::audio::MapAudioScene;
    using phoenix::audio::build_audible_tracks_into;
    using phoenix::audio::build_map_audio_scene;
    using phoenix::runtime::HeightSamplerContext;
    using phoenix::runtime::character_collision_callback;
    using phoenix::runtime::character_height_sampler;
    using phoenix::runtime::find_dungeon_playable_spawn;
    using phoenix::runtime::find_initial_playable_spawn;
    using phoenix::renderer::CameraView;
    using phoenix::renderer::ScreenPoint;
    using phoenix::renderer::build_visible_animated_batches;
    using phoenix::renderer::build_visible_object_batches;
    using phoenix::renderer::build_visible_terrain_ranges;
    using phoenix::renderer::project_world_to_screen;
    using phoenix::renderer::sort_scene_front_to_back;
    using phoenix::renderer::sphere_visible;
    using phoenix::character::BotManager;
    using phoenix::character::MonsterManager;
    using phoenix::character::NpcManager;
    using phoenix::character::scan_bot_equipment_pools;
    using phoenix::character::scan_character_options;

    constexpr const char* kAppTitle = "Phoenix Client";
    constexpr std::size_t kSkyTextureLayer = 63;
    constexpr std::size_t kPrimaryCloudTextureLayer = 64;
    constexpr std::size_t kSecondaryCloudTextureLayer = 65;
    constexpr std::size_t kAssetTextureLayerBase = 66;
    // A 384-layer pool was too small for the predictive ring on asset-dense
    // fields: geometry whose last texture could not lease a layer was dropped.
    // 768 remains bounded while covering those resident working sets.
    constexpr std::size_t kWorldAssetTextureSlotReserve = 768;
    constexpr std::uint32_t kWorldAssetTextureSize = 256;
    constexpr std::uint32_t kWorldAssetTextureMipLevels = 7;
    constexpr std::size_t kCharacterTextureSlotReserve = 32;
    constexpr std::size_t kBotTextureSlotReserve = 256;
    constexpr std::size_t kNpcTextureSlotReserve = 96;
    constexpr std::size_t kMonsterTextureSlotReserve = 256;

}

int main(int, char**)
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    const auto executableDir = phoenix::app::executable_directory();

    phoenix::platform::SdlWindow window;
    if (!window.create(kWidth, kHeight, kAppTitle))
    {
        std::fprintf(stderr, "Could not create Phoenix Client window.\n");
        return 1;
    }

    const auto [clientWidth, clientHeight] = window.client_size();
    phoenix::renderer::OpenGLRenderer renderer;
    if (!renderer.initialize(
        window.handle(),
        static_cast<std::uint32_t>(clientWidth > 0 ? clientWidth : kWidth),
        static_cast<std::uint32_t>(clientHeight > 0 ? clientHeight : kHeight)))
    {
        std::fprintf(stderr, "Could not initialize OpenGL.\n");
        return 1;
    }

    const auto uiAvailable = renderer.native_ui_available();

    phoenix::app::LoadingScheduler cpuLoader(phoenix::app::LoadingWorkKind::Cpu);
    phoenix::app::LoadingScheduler ioLoader(phoenix::app::LoadingWorkKind::Io);
    PerfHudState perfHud;
    perfHud.initialize_system_info();
    perfHud.gpuName = renderer.adapter_name();
    perfHud.renderer = &renderer;
    perfHud.antialiasingAvailable = window.has_multisample_context();
    phoenix::ui::GraphicsSettings graphicsSettings{};
    phoenix::ui::register_app_settings(
        graphicsSettings.worldShadows,
        graphicsSettings.terrainCompatibility,
        graphicsSettings.vsyncEnabled,
        perfHud.fpsCapIndex,
        perfHud.antialiasingEnabled);
    if (!renderer.set_vsync_enabled(graphicsSettings.vsyncEnabled))
        graphicsSettings.vsyncEnabled = false;
    renderer.set_antialiasing_enabled(perfHud.antialiasingAvailable && perfHud.antialiasingEnabled);
    renderer.set_shadows_enabled(graphicsSettings.worldShadows);
    renderer.set_terrain_compatibility(graphicsSettings.terrainCompatibility);

    // Closing the window must work even mid-load: skip all teardown and let
    // the OS reclaim the process — waiting on loaders/GPU only delays the user.
    auto pumpQuitCheck = [&]() {
        if (!window.pump_messages())
        {
            // Hide first so the close FEELS instant — the OS then reclaims the
            // process (potentially gigabytes) with no window on screen.
            window.hide();
            phoenix::ui::flush_app_settings();
            std::_Exit(0);
        }
    };

    auto showLoading = [&](float /*progress*/, std::string_view /*stage*/) {
        pumpQuitCheck();
        window.set_title(kAppTitle);
        const auto [sw, sh] = window.client_size();
        if (window.is_minimized() || sw <= 0 || sh <= 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
        else
        {
            const auto width = static_cast<std::uint32_t>(sw);
            const auto height = static_cast<std::uint32_t>(sh);
            renderer.resize(width, height);
            auto image = make_loading_image(width, height);
            renderer.set_preview_image(width, height, image);
            renderer.render_frame();
        }
    };

    // Run a function on a background thread while keeping the window responsive.
    auto runAsync = [&](auto fn, float progress, std::string_view stage) {
        auto future = cpuLoader.submit(std::move(fn));
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
            showLoading(progress, stage);
        return future.get();
    };

    // Overload for void-returning functions.
    auto runAsyncVoid = [&](auto fn, float progress, std::string_view stage) {
        auto future = cpuLoader.submit(std::move(fn));
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready)
            showLoading(progress, stage);
        future.get();
    };

    showLoading(0.03f, "Starting");

    phoenix::runtime::PhoenixRuntime runtime;
    runAsyncVoid([&]() { runtime.initialize(executableDir, false); }, 0.06f, "Indexing data");
    const auto reservedDynamicLayers = kCharacterTextureSlotReserve + kBotTextureSlotReserve
        + kNpcTextureSlotReserve + kMonsterTextureSlotReserve;
    const auto arrayLayerLimit = renderer.max_texture_array_layers();
    const auto maxWorldAssetTextureLayers = arrayLayerLimit > kAssetTextureLayerBase + reservedDynamicLayers
        ? static_cast<std::size_t>(arrayLayerLimit - kAssetTextureLayerBase - reservedDynamicLayers)
        : 1u;
    runtime.set_asset_texture_capacity(static_cast<std::uint32_t>(maxWorldAssetTextureLayers));
    showLoading(0.12f, "Indexing data");

    // Character system. Its catalog preload runs after the world load (each phase
    // is internally multithreaded, so they get full CPU cores rather than fighting
    // each other if overlapped).
    phoenix::character::CharacterSystem characterSystem;
    phoenix::character::CharacterAppearance characterAppearance{};
    BotManager botManager;
    MonsterManager monsterManager;
    NpcManager npcManager;

    std::size_t defaultMap{};
    const auto& startupMaps = runtime.world_map_names();
    for (std::size_t i = 0; i < startupMaps.size(); ++i)
    {
        if (startupMaps[i] == "world1")
        {
            defaultMap = i;
            break;
        }
    }
    if (!startupMaps.empty())
    {
        runAsyncVoid([&]() { runtime.load_world_map(defaultMap); }, 0.17f, "Loading world");
    }
    showLoading(0.24f, "World ready");

    phoenix::audio::AudioSystem audioSystem;
    const bool audioAvailable = audioSystem.initialize();
    showLoading(0.27f, "Audio");

    auto characterOptions = runAsync([&]() {
        return scan_character_options(runtime.state().assets.root);
    }, 0.29f, "Scanning characters");
    auto botEquipmentPools = runAsync([&]() {
        return scan_bot_equipment_pools(runtime.state().assets.root);
    }, 0.31f, "Scanning equipment");
    npcManager.load_catalog(runtime.state().assets.root);
    monsterManager.load_catalog(runtime.state().assets.root);
    // Character and item meshes are resolved on demand. Preloading every race,
    // animation and weapon retained roughly 800 MiB of session-global CPU data
    // shortly after the first map appeared, obscuring map teardown and making
    // the first reload look like a duplicated world.
    showLoading(0.32f, "Characters");
    int selectedCharacterOption = 0;
    for (std::size_t i = 0; i < characterOptions.size(); ++i)
    {
        if (characterOptions[i].raceFolder == characterAppearance.raceFolder
            && characterOptions[i].prefix == characterAppearance.prefix)
        {
            selectedCharacterOption = static_cast<int>(i);
            break;
        }
    }
    bool characterLoaded = false;
    bool playableMode = true; // true = third-person playable, false = free camera viewer

    bool fogEnabled = true;
    float viewDistance = 300.0f;
    float npcViewDistance = 100.0f;
    float monsterViewDistance = 100.0f;
    std::vector<phoenix::renderer::TerrainVertex> characterShadowScratch;
    WeatherMode weatherMode = WeatherMode::Default;
    // fogCullDistance is the actual cull boundary: nothing beyond the fog-end is
    // visible, so nothing beyond it should be rendered.
    float fogCullDistance = viewDistance;
    const auto applyFogSettings = [&]() {
        fogCullDistance = apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);
    };
    applyFogSettings();
    int pendingEmote = 0;   // emote triggered from the debug panel, consumed next frame
    std::size_t pendingAnimation = 0; // animation test triggered from the debug panel

    std::uint32_t terrainVertexCount{};
    std::uint32_t terrainIndexCount{};
    phoenix::runtime::PhoenixRuntime::TerrainLodInfo terrainLodInfo;
    float terrainResidencyCenterX{};
    float terrainResidencyCenterZ{};
    float terrainResidencyDistance{};
    std::vector<phoenix::renderer::TerrainDrawRange> visibleTerrainRanges;
    std::vector<phoenix::renderer::ObjectBatch> visibleBatches;
    std::vector<phoenix::renderer::ObjectBatch> visibleAnimatedBatches;
    std::uint32_t objectInstanceCount{};
    std::uint32_t objectBatchCount{};
    bool playMapSounds = true;
    bool playMapMusic = true;
    float masterVolume = 0.10f;
    int selectedMapIndex = static_cast<int>(runtime.selected_world_map());
    bool terrainTexturesUploaded = false;
    std::size_t characterTextureBaseSlot = 0;
    phoenix::runtime::StaticObjectScene staticObjectScene;
    phoenix::runtime::AnimatedObjectScene animatedObjectScene;
    phoenix::runtime::EffectParticleSystem effectParticleSystem;
    phoenix::runtime::WorldStreamer worldStreamer;
    std::optional<std::future<StreamedWorldScene>> streamedSceneBuild;
    std::optional<std::future<StreamedTerrainScene>> streamedTerrainBuild;
    phoenix::runtime::WeatherParticleSystem weatherParticleSystem;
    // Debug "Effects" panel: the full data/effects catalog (built once, lazily,
    // since it's independent of the loaded map), the currently selected file's
    // loaded/resolved library (cached by path so re-selecting is instant), and
    // the sequence-name list for whichever file is selected.
    std::vector<std::filesystem::path> effectLibraryFilePaths;
    std::vector<std::string> effectFileNames;
    std::unordered_map<std::string, DebugEffectLibraryEntry> effectLibraryCache;
    // Paths are the persistent source of truth. DDS payloads are only transient
    // upload staging; keeping every mip in RAM after the GPU upload retained
    // hundreds of MiB for the entire session.
    std::vector<std::filesystem::path> debugEffectTexturePaths;
    std::vector<phoenix::renderer::DdsTexture> debugEffectTexturesRaw;
    int selectedEffectFileIndex = 0;
    int selectedEffectSequenceIndex = 0;
    int loadedEffectFileIndex = -1;
    std::vector<std::string> effectSequenceNames;
    float effectSpawnYOffset = 0.0f;

    // Bots occasionally cast a random spell-ish effect (see
    // botManager.onCastEffect below) — restricted to files that are actually
    // used by real spells (in_/da_/ca_ name prefixes), a subset of
    // effectLibraryFilePaths filtered once alongside it.
    std::vector<std::filesystem::path> botEffectFilePaths;
    struct PendingBotEffect { std::uint32_t spawnId; float age; };
    std::vector<PendingBotEffect> pendingBotEffects;
    std::mt19937 botCastRng{ std::random_device{}() };
    // Diagnostic isolation: raw component index (library.effects), spawned
    // directly via the panel's "Spawn component" button, bypassing sequence
    // expansion.
    int effectComponentIndex = 0;
    phoenix::runtime::WorldCollisionMesh worldCollisionMesh;
    HeightSamplerContext heightSamplerCtx{ &runtime, &worldCollisionMesh };
    // Map mobs follow the terrain/floor as they spawn and wander.
    monsterManager.set_height_sampler(character_height_sampler, &heightSamplerCtx);
    MapAudioScene mapAudioScene;
    bool forceVisibilityUpdate = true;
    std::optional<std::size_t> pendingMapLoad;
    CameraView lastCullView{};

    const auto uploadCharacterMesh = [&]() {
        if (!characterLoaded || !characterSystem.ready())
            return;
        phoenix::app::set_character_mesh(renderer, characterSystem, playableMode,
            graphicsSettings.worldShadows);
    };

    const auto releaseDecodedTextureRam = [](std::vector<phoenix::renderer::DdsTexture>& textures) {
        for (auto& texture : textures)
        {
            std::vector<std::uint8_t>().swap(texture.rgba);
        }
    };

    // Normalise all textures to uniform BC3 format at the dominant resolution
    // so they can be uploaded as a single GPU-compressed Texture2DArray.
    const auto normalizeTexturesForBcUpload = [&](
        std::vector<phoenix::renderer::DdsTexture>& textures,
        std::string_view loadingStage = {},
        float progressStart = 0.0f,
        float progressEnd = 0.0f,
        // Caps the generated mip chain. Regular game assets always ship
        // with pre-baked mips through the production data pipeline, so
        // this only matters for content normalised on the fly here (the
        // debug "Effects" panel's textures): mip generation box-filters 2x2
        // neighborhoods repeatedly, which is fine for a normal single-image
        // texture but bleeds colors/alpha across cell boundaries for a
        // texture atlas (several distinct regions packed into one image,
        // common for effect flipbooks/animation frames) — visible as
        // "squares" of wrong transparency at any mip level beyond 0. Passing
        // 1 here disables mip generation entirely for that case.
        std::uint32_t maxMipLevels = UINT32_MAX) {
        if (textures.empty())
            return;

        // Vote on the dominant resolution among valid textures.
        std::map<std::uint64_t, std::uint32_t> sizeCounts;
        for (const auto& t : textures)
        {
            if (!t.valid || t.width == 0 || t.height == 0) continue;
            const auto key = (static_cast<std::uint64_t>(t.width) << 32) | t.height;
            sizeCounts[key]++;
        }
        std::uint32_t targetW = 256, targetH = 256; // default
        std::uint32_t bestCount = 0;
        for (const auto& [key, count] : sizeCounts)
        {
            if (count > bestCount)
            {
                bestCount = count;
                targetW = static_cast<std::uint32_t>(key >> 32);
                targetH = static_cast<std::uint32_t>(key & 0xFFFFFFFF);
            }
        }

        // Compute mip count: down to 4x4 blocks (same as renderer logic).
        const auto maxDim = std::max(targetW, targetH);
        const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
        const auto targetMips = std::min({ fullMips,
            static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)),
            std::max(1u, maxMipLevels) });

        // The data tree uses canonical BC3 textures with full mip chains, so
        // conversion is the exception. Collect only non-canonical entries
        // (foreign data,
        // broken files, empty reserved slots that need a fallback fill) and
        // convert just those.
        std::vector<std::size_t> pending;
        for (std::size_t idx = 0; idx < textures.size(); ++idx)
        {
            const auto& t = textures[idx];
            // Invalid entries (empty reserved slots, missing files) are left
            // as-is: the BC-native upload substitutes cheap fallback mips.
            if (!t.valid)
                continue;
            const bool canonical = t.compressed
                && t.vkFormat == phoenix::renderer::kDdsFormatBc3UnormBlock
                && t.width == targetW && t.height == targetH
                && t.mipData.size() >= targetMips;
            if (!canonical)
                pending.push_back(idx);
        }
        if (pending.empty())
            return;

        const auto workerCount = std::min(pending.size(), cpuLoader.worker_count());
        if (workerCount <= 1)
        {
            for (std::size_t i = 0; i < pending.size(); ++i)
            {
                phoenix::renderer::convert_texture_to_bc3(textures[pending[i]], targetW, targetH, targetMips);
                if (!loadingStage.empty() && (i % 4u == 0u || i + 1u == pending.size()))
                {
                    const float t = static_cast<float>(i + 1u) / static_cast<float>(pending.size());
                    showLoading(progressStart + (progressEnd - progressStart) * t, loadingStage);
                }
            }
            return;
        }

        std::atomic<std::size_t> nextTex{ 0 };
        std::atomic<std::size_t> completedTex{ 0 };
        std::vector<std::future<void>> futures;
        futures.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker)
        {
            futures.push_back(cpuLoader.submit([&]() {
                for (;;)
                {
                    const auto idx = nextTex.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= pending.size())
                        break;
                    phoenix::renderer::convert_texture_to_bc3(textures[pending[idx]], targetW, targetH, targetMips);
                    completedTex.fetch_add(1, std::memory_order_relaxed);
                }
            }));
        }

        while (completedTex.load(std::memory_order_relaxed) < pending.size())
        {
            if (!loadingStage.empty())
            {
                const float t = static_cast<float>(completedTex.load(std::memory_order_relaxed)) / static_cast<float>(pending.size());
                showLoading(progressStart + (progressEnd - progressStart) * t, loadingStage);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        for (auto& future : futures)
            future.get();
    };

    // True whenever debugEffectTexturePaths has grown since the last upload —
    // checked once per frame (see the flush next to the pendingBotEffects
    // sweep) instead of uploading inline in ensureEffectLibraryLoaded. Doing
    // it inline meant every newly-discovered file — whether from a bot
    // casting or the "Effects" panel's file picker — triggered its own full
    // re-normalize + re-upload of the WHOLE debug texture array (all layers
    // share one format/resolution, so there's no way to append just one).
    // A burst of bots each discovering a different file in the same frame
    // used to mean that many full-array rebuilds in a row, which is what
    // tanked framerate even with only a handful of effects actually playing.
    // Batching to at most one upload per frame fixes that without needing to
    // preload everything up front (which just moved the same cost to a
    // single big stall on the very first map load instead).
    bool debugEffectTexturesDirty = false;

    // Loads (and caches, by path) one data/effects file into a
    // DebugEffectLibraryEntry. Shared by the "Effects" panel's own file
    // picker and bots' random effect casting (see botManager.onCastEffect
    // below) so there's exactly one place that knows how to load an .eft
    // file on demand — texture upload is deferred (see debugEffectTexturesDirty).
    const auto ensureEffectLibraryLoaded = [&](const std::filesystem::path& path) -> DebugEffectLibraryEntry& {
        const auto key = path.string();
        auto cacheIt = effectLibraryCache.find(key);
        if (cacheIt == effectLibraryCache.end())
        {
            auto loaded = runtime.load_effect_library_file(path);

            DebugEffectLibraryEntry entry{};
            entry.library = std::move(loaded.library);
            entry.meshes = std::move(loaded.meshes);

            const auto baseLayer = debugEffectTexturePaths.size();
            entry.textureLayers.reserve(loaded.texturePaths.size());
            for (std::size_t i = 0; i < loaded.texturePaths.size(); ++i)
            {
                const auto& texturePath = loaded.texturePaths[i];
                if (texturePath.empty())
                {
                    entry.textureLayers.push_back(0xFFFFFFFFu);
                    debugEffectTexturePaths.emplace_back();
                    continue;
                }
                entry.textureLayers.push_back(0x80000000u | static_cast<std::uint32_t>(baseLayer + i));
                debugEffectTexturePaths.push_back(texturePath);
                debugEffectTexturesDirty = true;
            }

            cacheIt = effectLibraryCache.emplace(key, std::move(entry)).first;
        }
        return cacheIt->second;
    };

    // Bots occasionally cast: pick a random in_/da_/ca_ effect file, use
    // sequence 1 if there's more than one option (else the only one there
    // is), always one-shot. spawn_debug_effect() already handles culling
    // (see EffectParticleSystem::process_emitter's cameraPosition/
    // cullDistance check) the same way every other effect does — nothing
    // extra needed here for that. The returned id is tracked so it can be
    // force-cleaned at 10s if it doesn't finish on its own (see the
    // pendingBotEffects sweep next to botManager.update()).
    botManager.onCastEffect = [&](float x, float y, float z, float yaw) {
        // This is deliberately lazy. Preloading every castable effect in the
        // background retained a large CPU/GPU texture library unrelated to
        // the current map and made the first map reload appear to double RAM.
        // botEffectFilePaths is capped when the catalog is built, so demand
        // loading also has a hard upper memory bound.
        if (botEffectFilePaths.empty())
            return;
        const auto& path = botEffectFilePaths[botCastRng() % botEffectFilePaths.size()];
        auto& entry = ensureEffectLibraryLoaded(path);
        if (!entry.library.parsed || entry.library.sequences.empty())
            return;
        const std::int32_t sequenceIndex = entry.library.sequences.size() > 1 ? 1 : 0;
        const float position[3]{ x, y, z };
        const auto id = effectParticleSystem.spawn_debug_effect(
            entry.library, entry.textureLayers, entry.meshes, sequenceIndex, position, yaw, /*oneShot=*/true);
        if (id != 0)
            pendingBotEffects.push_back({ id, 0.0f });
    };

    std::size_t botTextureBaseSlot = 0;
    std::size_t npcTextureBaseSlot = 0;
    std::size_t monsterTextureBaseSlot = 0;
    std::optional<std::future<std::vector<phoenix::renderer::DdsTexture>>> botPresetBuild;
    int pendingBotSpawnCount = 0;

    const auto reloadCharacterIntoRenderer = [&]() {
        if (characterTextureBaseSlot == 0 || !terrainTexturesUploaded)
            return false;

        characterLoaded = characterSystem.load(
            runtime.state().assets.root, characterAppearance, /*allowPreload=*/false);
        if (!characterLoaded)
            return false;

        characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
        characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
        const auto& charTexPaths = characterSystem.texture_paths();
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));

        // Load the handful of swap textures from disk; with the canonical BC3
        // data tree the normalisation below is a pure pass-through.
        std::vector<phoenix::renderer::DdsTexture> characterTextures(charTexPaths.size());
        for (std::size_t i = 0; i < charTexPaths.size(); ++i)
            characterTextures[i] = phoenix::renderer::load_dds(charTexPaths[i]);
        normalizeTexturesForBcUpload(characterTextures);
        renderer.upload_terrain_texture_layers(static_cast<std::uint32_t>(characterTextureBaseSlot), characterTextures);

        // Fast mesh update: reuses existing GPU buffers, no vkDeviceWaitIdle.
        phoenix::app::update_character_mesh(renderer, characterSystem, playableMode,
            graphicsSettings.worldShadows);
        return true;
    };

    const auto uploadCurrentWorld = [&]() {
        renderer.enter_loading_mode();
        showLoading(0.36f, "Preparing scene");
        applyFogSettings();
        mapAudioScene = build_map_audio_scene(runtime);
        const auto texturePaths = runtime.terrain_texture_paths();
        const auto waterTexturePath = runtime.water_texture_path();
        const bool dungeonEnvironment = runtime.state().world.isDungeon;
        // Dungeons use a pure-black environment. Do not resolve, decode or
        // upload outdoor sky/cloud resources just because their WLD fields
        // happen to contain inherited names.
        const auto skyTexturePath = dungeonEnvironment
            ? std::filesystem::path{}
            : runtime.texture_path_for(runtime.state().world.skyFileName);
        const auto primaryCloudTexturePath = dungeonEnvironment
            ? std::filesystem::path{}
            : runtime.texture_path_for(runtime.state().world.primaryCloudFileName);
        const auto secondaryCloudTexturePath = dungeonEnvironment
            ? std::filesystem::path{}
            : runtime.texture_path_for(runtime.state().world.secondaryCloudFileName);
        const auto& assetTexturePaths = runtime.asset_texture_paths();

        runtime.load_water_animation();
        const auto& waterAnim = runtime.water_animation();
        // Water frames live between the asset pool and dynamic characters, so
        // account for them before fixing the immutable array layout.
        const auto maxSafeWorldAssetTextureLayers = arrayLayerLimit
                > kAssetTextureLayerBase + waterAnim.frameCount + reservedDynamicLayers
            ? static_cast<std::size_t>(arrayLayerLimit - kAssetTextureLayerBase
                - waterAnim.frameCount - reservedDynamicLayers)
            : 1u;
        const auto assetTextureLayerCount = runtime.uses_world_asset_streaming()
            ? std::min(maxSafeWorldAssetTextureLayers,
                std::max(kWorldAssetTextureSlotReserve, assetTexturePaths.size()))
            : assetTexturePaths.size();
        const auto waterLayout = phoenix::app::make_water_resource_layout(
            kAssetTextureLayerBase,
            assetTextureLayerCount,
            waterAnim.frameCount);
        characterTextureBaseSlot = waterLayout.firstFreeLayer;
        std::vector<phoenix::renderer::DdsTexture> terrainTextures(characterTextureBaseSlot);

        std::vector<phoenix::app::TextureLoadJob> jobs;
        jobs.reserve(texturePaths.size() + 4 + assetTexturePaths.size() + waterAnim.frameCount);
        for (std::size_t i = 0; i < texturePaths.size(); ++i)
            jobs.push_back({ i, texturePaths[i] });
        phoenix::app::append_water_texture_jobs(jobs, waterTexturePath, waterAnim, waterLayout);
        if (!skyTexturePath.empty())
            jobs.push_back({ kSkyTextureLayer, skyTexturePath });
        if (!primaryCloudTexturePath.empty())
            jobs.push_back({ kPrimaryCloudTextureLayer, primaryCloudTexturePath });
        if (!secondaryCloudTexturePath.empty())
            jobs.push_back({ kSecondaryCloudTextureLayer, secondaryCloudTexturePath });
        for (std::size_t i = 0; i < assetTexturePaths.size(); ++i)
            jobs.push_back({ kAssetTextureLayerBase + i, assetTexturePaths[i] });

        const auto textureWorkerCount = std::min(ioLoader.worker_count(), jobs.size());
        if (!jobs.empty())
        {
            std::atomic<std::size_t> nextJob{ 0 };
            std::atomic<std::size_t> completedJobs{ 0 };
            auto worker = [&]() {
                for (;;)
                {
                    const auto idx = nextJob.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= jobs.size())
                        break;
                    terrainTextures[jobs[idx].slot] = phoenix::renderer::load_dds(jobs[idx].path);
                    completedJobs.fetch_add(1, std::memory_order_relaxed);
                }
            };
            std::vector<std::future<void>> futures;
            futures.reserve(textureWorkerCount);
            for (std::size_t t = 0; t < textureWorkerCount; ++t)
                futures.push_back(ioLoader.submit(worker));
            while (completedJobs.load(std::memory_order_relaxed) < jobs.size())
            {
                const float textureProgress = static_cast<float>(completedJobs.load(std::memory_order_relaxed)) / static_cast<float>(jobs.size());
                showLoading(0.40f + textureProgress * 0.26f, "Loading textures");
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }
            for (auto& future : futures)
                future.get();
            showLoading(0.66f, "Textures ready");
        }

        {
            // Harmless workaround for broken terrain layers: a missing/corrupt
            // layer DDS used to render as a solid error colour. Clone the first
            // valid terrain layer instead so the section still looks like ground.
            std::size_t firstValidLayer = texturePaths.size();
            for (std::size_t i = 0; i < texturePaths.size(); ++i)
            {
                if (terrainTextures[i].valid)
                {
                    firstValidLayer = i;
                    break;
                }
            }
            if (firstValidLayer < texturePaths.size())
            {
                for (std::size_t i = 0; i < texturePaths.size(); ++i)
                {
                    if (!terrainTextures[i].valid)
                        terrainTextures[i] = terrainTextures[firstValidLayer];
                }
            }

        }

        const bool skyReady = !skyTexturePath.empty() && terrainTextures[kSkyTextureLayer].valid;
        const bool primaryCloudReady = !primaryCloudTexturePath.empty() && terrainTextures[kPrimaryCloudTextureLayer].valid;
        const bool secondaryCloudReady = !secondaryCloudTexturePath.empty() && terrainTextures[kSecondaryCloudTextureLayer].valid;
        renderer.set_sky_texture_layers(
            skyReady ? static_cast<std::uint32_t>(kSkyTextureLayer) : UINT32_MAX,
            primaryCloudReady ? static_cast<std::uint32_t>(kPrimaryCloudTextureLayer) : UINT32_MAX,
            secondaryCloudReady ? static_cast<std::uint32_t>(kSecondaryCloudTextureLayer) : UINT32_MAX);
        phoenix::app::configure_water_renderer(renderer, waterTexturePath, waterAnim, waterLayout);

        // Load character textures into the texture array after water frames.
        // Reserve character + bot slots so appearance swaps and bot spawns don't resize the GPU array.
        {
            botTextureBaseSlot = characterTextureBaseSlot + kCharacterTextureSlotReserve;
            npcTextureBaseSlot = botTextureBaseSlot + kBotTextureSlotReserve;
            monsterTextureBaseSlot = npcTextureBaseSlot + kNpcTextureSlotReserve;
            const auto reservedEnd = monsterTextureBaseSlot + kMonsterTextureSlotReserve;
            terrainTextures.resize(reservedEnd);

            const auto loadCharTextures = [&]() {
                phoenix::app::load_character_texture_slots(
                    characterSystem,
                    terrainTextures,
                    characterTextureBaseSlot,
                    kCharacterTextureSlotReserve);
            };

            if (!characterLoaded)
            {
                characterLoaded = runAsync([&]() {
                    return characterSystem.load(
                        runtime.state().assets.root, characterAppearance, /*allowPreload=*/false);
                }, 0.66f, "Preparing character");
                if (characterLoaded)
                {
                    characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
                    characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
                    loadCharTextures();
                }
            }
            else
            {
                loadCharTextures();
            }

        }

        showLoading(0.68f, "Uploading textures");
        if (!terrainTextures.empty())
        {
            showLoading(0.70f, "Normalising textures to BC3");
            normalizeTexturesForBcUpload(terrainTextures, "Normalising textures to BC3", 0.70f, 0.735f);

            showLoading(0.74f, "Uploading BC3 textures");
            terrainTexturesUploaded = renderer.upload_terrain_textures(terrainTextures, pumpQuitCheck,
                static_cast<std::uint32_t>(kAssetTextureLayerBase),
                static_cast<std::uint32_t>(assetTextureLayerCount));
            if (terrainTexturesUploaded)
            {
                releaseDecodedTextureRam(terrainTextures);
                // Also release mip data — GPU has it now.
                for (auto& t : terrainTextures)
                    std::vector<std::vector<std::uint8_t>>().swap(t.mipData);
            }
        }

        if (runtime.uses_world_asset_streaming())
        {
            // World-prop DDS files are authored at 256x256. Before streaming,
            // those files participated in the global array-size vote; once
            // they became lazy, terrain/effects incorrectly selected 128x128
            // and every streamed prop was destructively resampled. Keep the
            // resident prop pool at its canonical size without inflating the
            // much larger terrain/character array.
            renderer.configure_streamed_asset_texture_pool(
                static_cast<std::uint32_t>(kAssetTextureLayerBase),
                static_cast<std::uint32_t>(assetTextureLayerCount),
                kWorldAssetTextureSize, kWorldAssetTextureSize,
                kWorldAssetTextureMipLevels);
        }
        worldStreamer.configure(
            runtime,
            renderer,
            cpuLoader,
            static_cast<std::uint32_t>(kAssetTextureLayerBase),
            static_cast<std::uint32_t>(assetTextureLayerCount),
            assetTexturePaths);

        const auto& world = runtime.state().world;

        // Field alpha splat masks ("tonality" maps) — scanned before the
        // terrain-map upload so the shader knows which layers carry masks.
        std::uint32_t alphaMaskFlags = 0;
        std::vector<std::filesystem::path> alphaMaskPaths;
        if (!world.isDungeon)
            alphaMaskPaths = runtime.field_alpha_mask_paths(alphaMaskFlags);

        if (!world.terrainTextureMap.empty() && world.heightMapSide > 1)
        {
            std::vector<float> tileSizes;
            tileSizes.reserve(world.terrainLayers.size());
            for (const auto& layer : world.terrainLayers)
                tileSizes.push_back(std::max(1.0f, layer.tileSize));
            renderer.upload_terrain_texture_map(
                world.terrainTextureMap,
                world.heightMapSide,
                static_cast<float>(world.mapSize),
                tileSizes.data(),
                static_cast<std::uint32_t>(tileSizes.size()),
                alphaMaskFlags & ~1u, // bit 0 = base layer, never blended
                static_cast<std::uint32_t>(tileSizes.size()));
        }

        // Load and upload lightmaps:
        //  - field maps: <stem>_<sec>_l.dds (baked shadows + colour tones for
        //    the terrain) plus alpha splat masks appended as extra layers.
        //  - dungeons: <dgName>_L<i>.dds pages applied per-vertex through the
        //    static_object lightmap path (shadows + colour tones).
        {
            renderer.disable_field_lightmaps(); // reset before (re)upload per map
            std::uint32_t lmSectionCount = 0;
            auto lmPaths = runtime.field_lightmap_paths(lmSectionCount);
            bool dungeonLightmaps = false;
            if (lmPaths.empty())
            {
                lmPaths = runtime.dungeon_lightmap_paths();
                dungeonLightmaps = !lmPaths.empty();
                if (dungeonLightmaps)
                    lmSectionCount = 1;
            }
            if (!lmPaths.empty() && lmSectionCount > 0)
            {
                std::vector<phoenix::renderer::DdsTexture> lightmaps;
                lightmaps.reserve(lmPaths.size() + alphaMaskPaths.size());
                for (const auto& p : lmPaths)
                {
                    if (!p.empty() && std::filesystem::exists(p))
                        lightmaps.push_back(phoenix::renderer::load_dds(p));
                    else
                        lightmaps.push_back({}); // invalid placeholder (layer skipped)
                }

                // Texture arrays need uniform size/format; pages on disk vary
                // (DXT1/DXT3, mixed sizes). Normalise valid layers to BC3 at
                // the first valid page's dimensions.
                std::uint32_t lmW = 0, lmH = 0;
                for (const auto& t : lightmaps)
                {
                    if (t.valid && t.width > 0 && t.height > 0)
                    {
                        lmW = t.width;
                        lmH = t.height;
                        break;
                    }
                }
                if (lmW > 0 && lmH > 0)
                {
                    const bool packMasks = !dungeonLightmaps
                        && (alphaMaskFlags & ~1u) != 0 && !alphaMaskPaths.empty();
                    const auto sectionTotal = lmSectionCount * lmSectionCount;
                    const auto pixelCount = static_cast<std::size_t>(lmW) * lmH;

                    // Reserve the packed-mask slots up front so workers can
                    // write into stable indices: [pages][section0 p0/p1][...].
                    const auto maskBase = lightmaps.size();
                    if (packMasks)
                        lightmaps.resize(maskBase + static_cast<std::size_t>(sectionTotal) * 2);

                    // Page conversion (decode+resize+encode 1024^2 BC3) and
                    // mask packing are seconds of CPU — run them across all
                    // cores while the main thread keeps pumping the window.
                    const auto pageCount = maskBase;
                    const auto jobCount = pageCount + (packMasks ? static_cast<std::size_t>(sectionTotal) : 0);
                    std::atomic<std::size_t> nextJob{ 0 };
                    std::atomic<std::size_t> doneJobs{ 0 };
                    const auto workerCount = std::min(jobCount, cpuLoader.worker_count());
                    std::vector<std::future<void>> lmFutures;
                    lmFutures.reserve(workerCount);
                    for (std::size_t w = 0; w < workerCount; ++w)
                    {
                        lmFutures.push_back(cpuLoader.submit([&]() {
                            for (;;)
                            {
                                const auto job = nextJob.fetch_add(1, std::memory_order_relaxed);
                                if (job >= jobCount)
                                    break;
                                if (job < pageCount)
                                {
                                    if (lightmaps[job].valid)
                                        phoenix::renderer::convert_texture_to_bc3(lightmaps[job], lmW, lmH, 1);
                                }
                                else
                                {
                                    // Pack section s: masks for terrain layers
                                    // 1..7 into two RGBA weight textures.
                                    constexpr auto kMasksPerSection = phoenix::runtime::PhoenixRuntime::kFieldAlphaMaskLayers;
                                    const auto s = static_cast<std::uint32_t>(job - pageCount);
                                    std::vector<std::uint8_t> packed[2];
                                    packed[0].assign(pixelCount * 4, 0);
                                    packed[1].assign(pixelCount * 4, 0);
                                    for (std::uint32_t n = 1; n < kMasksPerSection; ++n)
                                    {
                                        const auto& p = alphaMaskPaths[s * kMasksPerSection + n];
                                        if (p.empty())
                                            continue;
                                        const auto mask = phoenix::renderer::load_dds(p);
                                        if (!mask.valid)
                                            continue;
                                        auto rgba = phoenix::renderer::decode_texture_rgba(mask);
                                        if (rgba.empty())
                                            continue;
                                        if (mask.width != lmW || mask.height != lmH)
                                            rgba = phoenix::renderer::resize_rgba(rgba.data(), mask.width, mask.height, lmW, lmH);
                                        if (rgba.size() < pixelCount * 4)
                                            continue;
                                        const auto channel = (n - 1u) & 3u;
                                        auto& dst = packed[(n - 1u) >> 2u];
                                        for (std::size_t px = 0; px < pixelCount; ++px)
                                            dst[px * 4 + channel] = rgba[px * 4 + 3]; // weight lives in alpha
                                    }
                                    for (std::uint32_t part = 0; part < 2; ++part)
                                    {
                                        auto& tex = lightmaps[maskBase + static_cast<std::size_t>(s) * 2 + part];
                                        tex = {};
                                        tex.valid = true;
                                        tex.compressed = true;
                                        tex.vkFormat = phoenix::renderer::kDdsFormatBc3UnormBlock;
                                        tex.blockBytes = 16;
                                        tex.width = lmW;
                                        tex.height = lmH;
                                        tex.mipData.assign(1, phoenix::renderer::encode_rgba_to_bc3(packed[part].data(), lmW, lmH));
                                    }
                                }
                                doneJobs.fetch_add(1, std::memory_order_relaxed);
                            }
                        }));
                    }
                    while (doneJobs.load(std::memory_order_relaxed) < jobCount)
                    {
                        pumpQuitCheck();
                        std::this_thread::sleep_for(std::chrono::milliseconds(8));
                    }
                    for (auto& future : lmFutures)
                        future.get();

                    renderer.upload_field_lightmaps(lightmaps, lmSectionCount);
                }
            }
        }

        std::vector<phoenix::renderer::TerrainVertex> terrainVertices;
        std::vector<std::uint32_t> terrainIndices;
        std::vector<phoenix::renderer::TerrainVertex> waterVertices;
        std::vector<std::uint32_t> waterIndices;

        terrainResidencyCenterX = 0.0f;
        terrainResidencyCenterZ = 0.0f;
        terrainResidencyDistance = std::max(420.0f, fogCullDistance + 192.0f);
        runAsyncVoid([&]() {
            if (runtime.uses_world_asset_streaming())
                runtime.build_streamed_terrain_mesh(terrainVertices, terrainIndices,
                    terrainLodInfo, terrainResidencyCenterX, terrainResidencyCenterZ,
                    terrainResidencyDistance);
            else
                runtime.build_terrain_mesh(terrainVertices, terrainIndices, terrainLodInfo);
        }, 0.74f, "Building nearby terrain");
        phoenix::app::build_water_mesh(
            static_cast<float>(std::max(1u, runtime.state().world.mapSize)),
            waterVertices,
            waterIndices);
        terrainVertexCount = static_cast<std::uint32_t>(terrainVertices.size());
        terrainIndexCount = static_cast<std::uint32_t>(terrainIndices.size());

        {
        }

        if (!renderer.set_terrain_mesh(terrainVertices, terrainIndices))
        {
            const auto previewWidth = std::max(480u, renderer.surface_width() / 2u);
            const auto previewHeight = std::max(270u, renderer.surface_height() / 2u);
            auto preview = runtime.create_3d_preview_image(previewWidth, previewHeight);
            renderer.set_preview_image(preview.width, preview.height, preview.bgra);
        }
        renderer.set_water_mesh(waterVertices, waterIndices);

        // Free terrain mesh CPU data — now in GPU buffers.
        { std::vector<phoenix::renderer::TerrainVertex>().swap(terrainVertices);
          std::vector<std::uint32_t>().swap(terrainIndices);
          std::vector<phoenix::renderer::TerrainVertex>().swap(waterVertices);
          std::vector<std::uint32_t>().swap(waterIndices); }

        // Settle only the spawn-area residency while the loading screen is
        // already visible. This mirrors Godot's initial-near settle: the map
        // becomes playable with its immediate collision/props present, while
        // every distant sector remains cold and continues to stream later.
        if (worldStreamer.active())
        {
            for (;;)
            {
                const auto streamed = worldStreamer.update(
                    terrainResidencyCenterX, terrainResidencyCenterZ,
                    fogCullDistance);
                showLoading(0.80f, "Streaming spawn area");
                if (streamed.pendingAssets == 0)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        }

        staticObjectScene = runAsync([&runtime]() {
            return runtime.build_static_object_scene();
        }, 0.82f, "Building static objects");
        animatedObjectScene = runAsync([&runtime]() {
            return runtime.build_animated_object_scene();
        }, 0.87f, "Building animated objects");
        effectParticleSystem.build(runtime.state());

        // The debug "Effects" panel's file catalog is independent of the map
        // (it lists all of data/effects), so build it once, lazily.
        if (effectLibraryFilePaths.empty())
        {
            effectLibraryFilePaths = runtime.effect_library_files();
            effectFileNames.reserve(effectLibraryFilePaths.size());
            for (std::size_t i = 0; i < effectLibraryFilePaths.size(); ++i)
                effectFileNames.push_back(std::to_string(i) + ": " + effectLibraryFilePaths[i].filename().string());

            // Bots only cast files actually used by real spells — the
            // in_/da_/ca_ name-prefix convention filters out map-decoration
            // effects (torches, fountains, ...) that happen to live in the
            // same data/effects folder.
            for (const auto& path : effectLibraryFilePaths)
            {
                auto stem = phoenix::assets::lower_ascii(path.stem().string());
                if (stem.starts_with("in_") || stem.starts_with("da_") || stem.starts_with("ca_"))
                    botEffectFilePaths.push_back(path);
            }

            // Bot effects are incidental ambience, not map content. Keep a
            // small randomized selection so repeated casts cannot gradually
            // populate the session with the whole effects library.
            constexpr std::size_t kMaxBotEffectLibraries = 4;
            std::shuffle(botEffectFilePaths.begin(), botEffectFilePaths.end(), botCastRng);
            if (botEffectFilePaths.size() > kMaxBotEffectLibraries)
                botEffectFilePaths.resize(kMaxBotEffectLibraries);

            // load_effect_library_file() resolves everything against the
            // session-wide DataIndex (state_.assets), not per-map data — so
            // unlike the main terrain/asset texture array, effectLibraryCache/
            // debugEffectTexturesRaw don't actually go stale across map
            // changes and this only needs to run once, ever.
        }

        loadedEffectFileIndex = -1;
        effectSequenceNames.clear();
        objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
        objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
        renderer.set_static_object_mesh(
            staticObjectScene.vertices,
            staticObjectScene.indices,
            staticObjectScene.instances,
            staticObjectScene.batches);

        // Upload animated object mesh to GPU. Always called, even when this map has
        // no vertex-animated (VANI) objects: set_animated_object_mesh() destroys the
        // old GPU buffers and clears animatedObjectsReady itself before checking for
        // empty input. Skipping this call when the new map's scene is empty used to
        // leave the PREVIOUS map's animated buffers (geometry + instances + ready
        // flag) untouched, so they kept rendering — stale foliage from whatever map
        // was loaded before, floating at that map's old coordinates in the new one.
        renderer.set_animated_object_mesh(
            animatedObjectScene.vertices,
            animatedObjectScene.indices,
            animatedObjectScene.instances,
            animatedObjectScene.batches);

        showLoading(0.90f, "Finalizing scene");

        worldCollisionMesh = runAsync([&]() { return runtime.build_collision_mesh(); }, 0.92f, "Building collision");

        // Climbable ladder volumes (WLD "Object" section) for the playable character.
        {
            const auto runtimeLadders = runtime.ladder_volumes();
            std::vector<phoenix::character::CharacterSystem::LadderVolume> ladders;
            ladders.reserve(runtimeLadders.size());
            for (const auto& ladder : runtimeLadders)
                ladders.push_back({ ladder.x, ladder.z, ladder.baseY, ladder.topY, ladder.radius });
            characterSystem.set_ladders(std::move(ladders));
        }

        // ---- Release CPU-side data already uploaded to GPU ----
        // Vertices and indices are now in GPU buffers; free CPU copies.
        {
            const auto releaseVector = [](auto& vec) {
                vec.clear();
                vec.shrink_to_fit();
            };
            releaseVector(staticObjectScene.vertices);
            releaseVector(staticObjectScene.indices);
        }

        // Upload character mesh (initial bind pose).
        if (characterLoaded && characterSystem.ready())
            uploadCharacterMesh();

        // ---- Initial playable spawn: centre of the map (valid interior point in
        // dungeons), snapped to a walkable surface.
        if (characterLoaded && characterSystem.ready())
        {
            const auto spawn = find_initial_playable_spawn(
                worldCollisionMesh, runtime.state().world.isDungeon);
            if (spawn.valid)
            {
                characterSystem.set_world_position(spawn.x, spawn.y, spawn.z, 0.0f);
                heightSamplerCtx.lastCharacterY = spawn.y;
                uploadCharacterMesh();
            }
        }

        // Load the NPCs authored in this map's .svmap (same stem as the .wld).
        // They stream in lazily as they enter camera range and keep their
        // authored Y (no terrain clamp).
        {
            const auto& mapPaths = runtime.state().worldMapPaths;
            const auto sel = runtime.selected_world_map();
            if (sel < mapPaths.size())
            {
                auto svmapPath = mapPaths[sel];
                svmapPath.replace_extension(".svmap");
                const float mapSize = static_cast<float>(runtime.state().world.mapSize);
                const float halfMap = runtime.state().world.isDungeon ? 0.0f : mapSize * 0.5f;
                npcManager.load_map_npcs(
                    runtime.state().assets.root, svmapPath, halfMap,
                    static_cast<std::uint32_t>(npcTextureBaseSlot),
                    static_cast<std::uint32_t>(kNpcTextureSlotReserve), renderer);
                monsterManager.load_map_monsters(
                    runtime.state().assets.root, svmapPath, halfMap,
                    static_cast<std::uint32_t>(monsterTextureBaseSlot),
                    static_cast<std::uint32_t>(kMonsterTextureSlotReserve), renderer);
            }
        }

        forceVisibilityUpdate = true;
        showLoading(1.0f, "Ready");

        phoenix::app::release_memory_to_os();
        // Commit presentation atomically: all earlier showLoading() calls stay
        // on the loading image even though terrain/water/object uploads have
        // already marked their individual renderer resources ready.
        renderer.leave_loading_mode();
    };

    uploadCurrentWorld();
    applyFogSettings();
    window.set_title(kAppTitle);

    using clock = std::chrono::steady_clock;
    auto lastFrame = clock::now();
    float totalTime = 0.0f;
    bool playToggleWasDown = false;
    bool orbitPointerWasDown = false;
    bool orbitCapturePending = false;
    bool orbitMouseCaptured = false;
    int orbitStartX = 0;
    int orbitStartY = 0;
    float orbitDragX = 0.0f;
    float orbitDragY = 0.0f;
    auto lastClientSize = window.client_size();
    bool windowWasMinimized = window.is_minimized();

    while (window.pump_messages())
    {
        const auto currentClientSize = window.client_size();
        const bool minimized = window.is_minimized() || currentClientSize.first <= 0 || currentClientSize.second <= 0;
        if (minimized)
        {
            if (!windowWasMinimized)
            {
                windowWasMinimized = true;
            }
            // While minimized the surface is 0x0 and the swapchain becomes
            // out-of-date. Force lastClientSize to an impossible value so that on
            // restore (which keeps the same size) the size-change check below fires
            // and recreates the swapchain — otherwise the window stays frozen.
            lastClientSize = { 0, 0 };
            lastFrame = clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        window.consume_restore_event();
        if (windowWasMinimized)
        {
            windowWasMinimized = false;
            renderer.recreate_swapchain();
            lastClientSize = window.client_size();
            lastFrame = clock::now();
        }
        if (currentClientSize != lastClientSize)
        {
            renderer.resize(
                static_cast<std::uint32_t>(currentClientSize.first),
                static_cast<std::uint32_t>(currentClientSize.second));
            lastClientSize = currentClientSize;
        }

        renderer.wait_for_frame();

        const auto now = clock::now();
        const auto deltaSeconds = std::chrono::duration<float>(now - lastFrame).count();
        lastFrame = now;
        totalTime += deltaSeconds;
        perfHud.push_frametime(deltaSeconds);
        renderer.update_water_time(totalTime);

        if (botPresetBuild
            && botPresetBuild->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto textures = botPresetBuild->get();
            botPresetBuild.reset();
            if (pendingBotSpawnCount <= 0)
            {
                botManager.clear();
            }
            else if (!textures.empty() && botManager.presetsBuilt)
            {
                renderer.upload_terrain_texture_layers(
                    static_cast<std::uint32_t>(botTextureBaseSlot), textures);
                if (characterSystem.ready())
                {
                    npcManager.clear(renderer);
                    botManager.spawn(pendingBotSpawnCount,
                        characterSystem.world_x(), characterSystem.world_z(),
                        character_height_sampler, &heightSamplerCtx);
                }
            }
            pendingBotSpawnCount = 0;
        }

        // Toggle playable mode with P key.
        const auto playToggleDown = window.is_key_down(SDLK_p);
        if (playToggleDown && !playToggleWasDown && characterLoaded)
        {
            playableMode = !playableMode;
            // Switching to free camera: start at the character's current position
            // so the view is continuous (not a jump to some fixed map position).
            if (!playableMode && characterSystem.ready())
            {
                float cx, cy, cz, cyaw, cpitch;
                characterSystem.camera_state(cx, cy, cz, cyaw, cpitch);
                runtime.set_camera_position(cx, cy, cz, cyaw, cpitch);
            }
            // Re-entering playable mode drops the character at the free-camera
            // location on every map, snapped to the nearest walkable collision surface.
            if (playableMode && characterSystem.ready())
            {
                float freeCamX{};
                float freeCamY{};
                float freeCamZ{};
                float freeCamYaw{};
                float freeCamPitch{};
                runtime.camera_state(freeCamX, freeCamY, freeCamZ, freeCamYaw, freeCamPitch);
                const auto spawn = find_dungeon_playable_spawn(worldCollisionMesh, freeCamX, freeCamY, freeCamZ);
                if (spawn.valid)
                {
                    characterSystem.set_world_position(spawn.x, spawn.y, spawn.z, freeCamYaw);
                    heightSamplerCtx.lastCharacterY = spawn.y;
                    uploadCharacterMesh();
                }
            }
            renderer.set_character_visible(playableMode);
            forceVisibilityUpdate = true;
        }
        playToggleWasDown = playToggleDown;

        phoenix::ui::px::begin_frame(window,
            static_cast<float>(renderer.surface_width()),
            static_cast<float>(renderer.surface_height()));
        const auto uiWantsKeyboard = uiAvailable && phoenix::ui::px::wants_keyboard();
        const auto uiWantsMouse = uiAvailable && phoenix::ui::px::wants_mouse();

        const auto [mouseDx, mouseDy] = window.consume_mouse_delta();
        const auto mouseWheel = window.consume_mouse_wheel_delta();

        const bool playableOrbitAvailable = playableMode && characterLoaded && characterSystem.ready();
        const bool orbitPointerDown = !uiWantsMouse
            && ((playableOrbitAvailable
                    && (window.is_mouse_button_down(0) || window.is_mouse_button_down(1)))
                || (!playableMode && window.is_mouse_button_down(1)));
        if (orbitPointerDown && !orbitPointerWasDown)
        {
            const auto [mouseX, mouseY] = window.mouse_position();
            orbitStartX = mouseX;
            orbitStartY = mouseY;
            orbitDragX = 0.0f;
            orbitDragY = 0.0f;
            orbitCapturePending = true;
        }
        if (orbitCapturePending && orbitPointerDown)
        {
            orbitDragX += static_cast<float>(mouseDx);
            orbitDragY += static_cast<float>(mouseDy);
            if (orbitDragX * orbitDragX + orbitDragY * orbitDragY >= 9.0f)
            {
                window.set_relative_mouse_mode(true);
                orbitMouseCaptured = true;
                orbitCapturePending = false;
            }
        }
        if (!orbitPointerDown && orbitPointerWasDown)
        {
            orbitCapturePending = false;
            if (orbitMouseCaptured)
            {
                window.set_relative_mouse_mode(false);
                window.warp_mouse(orbitStartX, orbitStartY);
                orbitMouseCaptured = false;
            }
        }
        orbitPointerWasDown = orbitPointerDown;

        float cameraX{};
        float cameraY{};
        float cameraZ{};
        float cameraYaw{};
        float cameraPitch{};

        if (playableMode && characterLoaded && characterSystem.ready())
        {
            // ---- Playable mode: third-person character control ----
            phoenix::character::PlayableInput pInput{};
            if (!uiWantsKeyboard)
            {
                pInput.forward = window.is_key_down(SDLK_w);
                pInput.backward = window.is_key_down(SDLK_s);
                pInput.left = window.is_key_down(SDLK_a);
                pInput.right = window.is_key_down(SDLK_d);
                pInput.jump = window.is_key_down(SDLK_SPACE);
                // Running is the default gait; holding Shift walks.
                pInput.fast = !window.is_key_down(SDLK_LSHIFT);
                pInput.yawLeft = window.is_key_down(SDLK_LEFT);
                pInput.yawRight = window.is_key_down(SDLK_RIGHT);
                pInput.pitchUp = window.is_key_down(SDLK_UP);
                pInput.pitchDown = window.is_key_down(SDLK_DOWN);
                pInput.sit = window.is_key_down(SDLK_c);
            }
            pInput.cameraOrbit = orbitMouseCaptured;
            pInput.cameraTurn = orbitMouseCaptured && window.is_mouse_button_down(1);
            pInput.mouseDx = orbitMouseCaptured ? static_cast<float>(mouseDx) : 0.0f;
            pInput.mouseDy = orbitMouseCaptured ? static_cast<float>(mouseDy) : 0.0f;
            pInput.mouseWheel = !uiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

            // Apply a pending emote set by the previous debug-panel frame.
            pInput.emote = pendingEmote;
            pendingEmote = 0;  // consumed
            pInput.debugAnimation = pendingAnimation;
            pendingAnimation = 0;  // consumed

            heightSamplerCtx.lastCharacterY = characterSystem.world_y();
            characterSystem.update(deltaSeconds, pInput);
            characterSystem.camera_state(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);

            if (!npcManager.active())
            {
                botManager.update(deltaSeconds, cameraX, cameraZ, cameraYaw, fogCullDistance,
                    character_height_sampler, &heightSamplerCtx, &cpuLoader);
            }

            // Safety net for bot-cast effects (see botManager.onCastEffect):
            // force-remove anything that's either finished on its own or has
            // been running for 10s regardless.
            for (std::size_t i = 0; i < pendingBotEffects.size();)
            {
                auto& pending = pendingBotEffects[i];
                pending.age += deltaSeconds;
                if (pending.age > 10.0f || !effectParticleSystem.debug_spawn_active(pending.spawnId))
                {
                    effectParticleSystem.remove_debug_spawn(pending.spawnId);
                    pending = pendingBotEffects.back();
                    pendingBotEffects.pop_back();
                }
                else
                {
                    ++i;
                }
            }

            phoenix::app::update_character_vertices(renderer, characterSystem,
                characterShadowScratch, graphicsSettings.worldShadows);
            if (!npcManager.active() && botManager.bots.empty())
            {
                renderer.set_bot_character_visible(false);
            }
            else if (!npcManager.active())
            {
                if (botManager.updatePoseMesh(renderer))
                {
                    botManager.updatePoseInstances(renderer);
                    renderer.set_bot_character_visible(true);
                }
            }
            else
            {
                // NPCs active: they render on their own GPU-skinned path now
                // (not the shared bot-character buffers), so hide any stale bots.
                renderer.set_bot_character_visible(false);
            }
        }
        else
        {
            // ---- Viewer mode: free camera ----
            phoenix::runtime::CameraInput cameraInput{};
            if (!uiWantsKeyboard)
            {
                cameraInput.forward = window.is_key_down(SDLK_w);
                cameraInput.backward = window.is_key_down(SDLK_s);
                cameraInput.left = window.is_key_down(SDLK_a);
                cameraInput.right = window.is_key_down(SDLK_d);
                cameraInput.up = window.is_key_down(SDLK_e) || window.is_key_down(SDLK_SPACE);
                cameraInput.down = window.is_key_down(SDLK_q) || window.is_key_down(SDLK_LCTRL);
                cameraInput.fast = window.is_key_down(SDLK_LSHIFT);
                cameraInput.yawLeft = window.is_key_down(SDLK_LEFT);
                cameraInput.yawRight = window.is_key_down(SDLK_RIGHT);
                cameraInput.pitchUp = window.is_key_down(SDLK_UP);
                cameraInput.pitchDown = window.is_key_down(SDLK_DOWN);
            }
            cameraInput.look = orbitMouseCaptured;
            cameraInput.mouseDx = orbitMouseCaptured ? static_cast<float>(mouseDx) : 0.0f;
            cameraInput.mouseDy = orbitMouseCaptured ? static_cast<float>(mouseDy) : 0.0f;
            cameraInput.wheel = !uiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

            const auto cameraChanged = cameraInput.forward || cameraInput.backward
                || cameraInput.left || cameraInput.right
                || cameraInput.up || cameraInput.down
                || cameraInput.yawLeft || cameraInput.yawRight
                || cameraInput.pitchUp || cameraInput.pitchDown
                || (cameraInput.look && (mouseDx != 0 || mouseDy != 0))
                || cameraInput.wheel != 0.0f;
            if (cameraChanged)
                runtime.update_camera(deltaSeconds, cameraInput);

            runtime.camera_state(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);
            renderer.set_character_visible(false);
        }

        // Flush any debug-effect textures discovered this frame (bot casts
        // and/or the "Effects" panel's file picker both funnel through
        // ensureEffectLibraryLoaded, which only marks this dirty — see its
        // definition). At most one full-array upload per frame regardless
        // of how many distinct new files got touched.
        if (debugEffectTexturesDirty)
        {
            if (debugEffectTexturesRaw.size() != debugEffectTexturePaths.size())
            {
                debugEffectTexturesRaw.clear();
                debugEffectTexturesRaw.resize(debugEffectTexturePaths.size());
                phoenix::app::parallel_for_loading(cpuLoader, debugEffectTexturePaths.size(), [&](std::size_t i) {
                    if (!debugEffectTexturePaths[i].empty())
                        debugEffectTexturesRaw[i] = phoenix::renderer::load_dds(debugEffectTexturePaths[i]);
                });
            }
            auto normalized = debugEffectTexturesRaw;
            normalizeTexturesForBcUpload(normalized, {}, 0.0f, 0.0f, 1u);
            renderer.upload_debug_effect_textures(normalized);
            std::vector<phoenix::renderer::DdsTexture>().swap(debugEffectTexturesRaw);
            phoenix::app::release_memory_to_os();
            debugEffectTexturesDirty = false;
        }

        // Godot feeds the continuously smoothed transform to Camera3D.  Preserve
        // it here as well: quantizing it to a 1/512 grid produced tiny but
        // perceptible steps while orbiting slowly.
        const float renderCameraX = cameraX;
        const float renderCameraY = cameraY;
        const float renderCameraZ = cameraZ;
        renderer.set_camera(
            renderCameraX,
            renderCameraY,
            renderCameraZ,
            cameraYaw,
            cameraPitch,
            static_cast<float>(std::max(1u, renderer.surface_width())) / static_cast<float>(std::max(1u, renderer.surface_height())),
            fogCullDistance);

        CameraView currentView{};
        currentView.x = renderCameraX;
        currentView.y = renderCameraY;
        currentView.z = renderCameraZ;
        currentView.yaw = cameraYaw;
        currentView.pitch = cameraPitch;
        currentView.aspect = static_cast<float>(std::max(1u, renderer.surface_width()))
            / static_cast<float>(std::max(1u, renderer.surface_height()));
        currentView.distance = fogCullDistance;

        if (streamedTerrainBuild
            && streamedTerrainBuild->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto terrain = streamedTerrainBuild->get();
            streamedTerrainBuild.reset();
            if (!terrain.vertices.empty() && !terrain.indices.empty())
            {
                renderer.set_terrain_mesh(terrain.vertices, terrain.indices);
                terrainVertexCount = static_cast<std::uint32_t>(terrain.vertices.size());
                terrainIndexCount = static_cast<std::uint32_t>(terrain.indices.size());
                terrainLodInfo = std::move(terrain.lod);
                terrainResidencyCenterX = terrain.centerX;
                terrainResidencyCenterZ = terrain.centerZ;
                terrainResidencyDistance = terrain.residentDistance;
                forceVisibilityUpdate = true;
            }
        }
        if (runtime.uses_world_asset_streaming() && !streamedTerrainBuild)
        {
            const float dx = currentView.x - terrainResidencyCenterX;
            const float dz = currentView.z - terrainResidencyCenterZ;
            const float chunkWidth = std::max(32.0f, terrainLodInfo.cellSize
                * static_cast<float>(phoenix::runtime::PhoenixRuntime::kTerrainChunkQuads));
            const float rebuildDistance = std::max(48.0f, chunkWidth * 0.75f);
            const float wantedDistance = std::max(420.0f, currentView.distance + 192.0f);
            if (dx * dx + dz * dz >= rebuildDistance * rebuildDistance
                || std::abs(wantedDistance - terrainResidencyDistance) >= 64.0f)
            {
                const float centerX = currentView.x;
                const float centerZ = currentView.z;
                streamedTerrainBuild.emplace(cpuLoader.submit(
                    [&runtime, centerX, centerZ, wantedDistance]() {
                        StreamedTerrainScene terrain{};
                        terrain.centerX = centerX;
                        terrain.centerZ = centerZ;
                        terrain.residentDistance = wantedDistance;
                        runtime.build_streamed_terrain_mesh(
                            terrain.vertices, terrain.indices, terrain.lod,
                            centerX, centerZ, wantedDistance);
                        return terrain;
                    }));
            }
        }

        // Also update when map (.svmap) NPCs are still waiting to stream in —
        // active() is false until the first one enters range, but update() is
        // what runs the streaming gate.
        if (npcManager.active() || npcManager.map_npc_streamed() < npcManager.map_npc_total())
        {
            auto npcView = currentView;
            npcView.distance = std::min(npcViewDistance, fogCullDistance);
            npcManager.update(deltaSeconds, npcView, renderer, &cpuLoader);
        }
        // Also update while map mobs are still waiting to stream in (active() is
        // false until the first enters range, but update() runs the streaming).
        if (monsterManager.active()
            || monsterManager.map_monster_streamed() < monsterManager.map_monster_total())
        {
            auto monsterView = currentView;
            monsterView.distance = std::min(monsterViewDistance, fogCullDistance);
            monsterManager.update(deltaSeconds, monsterView, renderer, &cpuLoader);
        }

        // Models, collision, instances and texture leases use real residency,
        // with a preload ring and wider unload hysteresis. Model/DDS work and
        // scene/collision assembly both stay off the render thread.
        if (streamedSceneBuild
            && streamedSceneBuild->wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            auto scene = streamedSceneBuild->get();
            streamedSceneBuild.reset();
            staticObjectScene = std::move(scene.staticObjects);
            animatedObjectScene = std::move(scene.animatedObjects);
            worldCollisionMesh = std::move(scene.collision);
            objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
            objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
            renderer.set_static_object_mesh(
                staticObjectScene.vertices, staticObjectScene.indices,
                staticObjectScene.instances, staticObjectScene.batches);
            renderer.set_animated_object_mesh(
                animatedObjectScene.vertices, animatedObjectScene.indices,
                animatedObjectScene.instances, animatedObjectScene.batches);

            std::vector<phoenix::character::CharacterSystem::LadderVolume> ladders;
            ladders.reserve(scene.ladders.size());
            for (const auto& ladder : scene.ladders)
                ladders.push_back({ ladder.x, ladder.z, ladder.baseY,
                    ladder.topY, ladder.radius });
            characterSystem.set_ladders(std::move(ladders));
            std::vector<phoenix::renderer::TerrainVertex>().swap(staticObjectScene.vertices);
            std::vector<std::uint32_t>().swap(staticObjectScene.indices);
            forceVisibilityUpdate = true;
        }
        if (worldStreamer.active() && !streamedSceneBuild)
        {
            const auto streamed = worldStreamer.update(
                currentView.x, currentView.z, currentView.distance);
            if (streamed.sceneChanged)
            {
                streamedSceneBuild.emplace(cpuLoader.submit([&runtime]() {
                    StreamedWorldScene scene{};
                    scene.staticObjects = runtime.build_static_object_scene();
                    scene.animatedObjects = runtime.build_animated_object_scene();
                    scene.collision = runtime.build_collision_mesh();
                    scene.ladders = runtime.ladder_volumes();
                    return scene;
                }));
            }
        }

        const auto cullMoveDx = currentView.x - lastCullView.x;
        const auto cullMoveDy = currentView.y - lastCullView.y;
        const auto cullMoveDz = currentView.z - lastCullView.z;
        const auto cullMoveSq = cullMoveDx * cullMoveDx + cullMoveDy * cullMoveDy + cullMoveDz * cullMoveDz;
        // Keep CPU-authored visibility close to the camera.  Large refresh
        // intervals leave an old frustum active while moving or turning and
        // make world batches appear to blink at the edge of the view.
        constexpr float kVisibilityMoveRefresh = 8.0f;
        constexpr float kVisibilityAngleRefresh = 0.02f;
        const bool visibilityNeedsUpdate = forceVisibilityUpdate
            || cullMoveSq > kVisibilityMoveRefresh * kVisibilityMoveRefresh
            || std::abs(currentView.yaw - lastCullView.yaw) > kVisibilityAngleRefresh
            || std::abs(currentView.pitch - lastCullView.pitch) > kVisibilityAngleRefresh
            || std::abs(currentView.distance - lastCullView.distance) > 32.0f
            || std::abs(currentView.aspect - lastCullView.aspect) > 0.01f;
        if (visibilityNeedsUpdate)
        {
            build_visible_terrain_ranges(visibleTerrainRanges, runtime, currentView, terrainLodInfo,
                graphicsSettings.terrainCompatibility);
            renderer.set_terrain_draw_ranges(visibleTerrainRanges);
            std::uint32_t visibleTerrainIndexCount{};
            for (const auto& range : visibleTerrainRanges)
                visibleTerrainIndexCount += range.indexCount;
            if (visibleTerrainIndexCount > 0)
                terrainIndexCount = visibleTerrainIndexCount;

            visibleBatches.clear();
            build_visible_object_batches(staticObjectScene, currentView, visibleBatches);
            renderer.set_static_object_batches(visibleBatches);
            std::uint32_t visibleObjectInstances{};
            for (const auto& batch : visibleBatches)
                visibleObjectInstances += batch.instanceCount;
            objectInstanceCount = visibleObjectInstances;
            objectBatchCount = static_cast<std::uint32_t>(visibleBatches.size());

            if (!animatedObjectScene.batches.empty())
            {
                visibleAnimatedBatches.clear();
                build_visible_animated_batches(animatedObjectScene, currentView, visibleAnimatedBatches);
                renderer.set_animated_object_batches(visibleAnimatedBatches);
                for (auto& animation : animatedObjectScene.vertexAnimations)
                {
                    animation.visible = std::ranges::any_of(visibleAnimatedBatches, [&](const auto& batch) {
                        return batch.firstIndex == animation.firstIndex && batch.indexCount == animation.indexCount;
                    });
                }
            }

            lastCullView = currentView;
            forceVisibilityUpdate = false;
        }

        if (!animatedObjectScene.vertexAnimations.empty())
        {
            runtime.update_animated_object_scene(animatedObjectScene, totalTime,
                renderCameraX, renderCameraY, renderCameraZ, fogCullDistance);
            animatedObjectScene.merge_dirty_ranges();
            for (const auto& range : animatedObjectScene.dirtyVertexRanges)
            {
                renderer.update_animated_object_vertices_range(
                    animatedObjectScene.vertices.data(),
                    range.firstVertex,
                    range.vertexCount);
            }
            animatedObjectScene.clear_dirty();
        }

        {
            const float cosYaw = std::cos(cameraYaw);
            const float sinYaw = std::sin(cameraYaw);
            const float cosPitch = std::cos(cameraPitch);
            const float sinPitch = std::sin(cameraPitch);
            // World-space camera basis, matching the yaw-then-pitch rotation
            // static_object.vert/effect_particle.vert apply to world deltas.
            const float cameraRightWorld[3]{ -cosYaw, 0.0f, sinYaw };
            const float cameraUpWorld[3]{ -sinPitch * sinYaw, cosPitch, -sinPitch * cosYaw };
            const float cameraForwardWorld[3]{ cosPitch * sinYaw, sinPitch, cosPitch * cosYaw };
            const float cameraPositionWorld[3]{ renderCameraX, renderCameraY, renderCameraZ };
            const float effectCullDistance = runtime.state().world.isDungeon
                ? std::min(fogCullDistance, 20.0f)
                : std::min(fogCullDistance, 70.0f);
            effectParticleSystem.update(deltaSeconds, cameraPositionWorld,
                cameraRightWorld, cameraUpWorld, cameraForwardWorld, effectCullDistance);

            // WeatherMode -> WeatherParticleSystem kind/intensity. Storm/
            // Snowstorm map to Heavy for now; Light/Medium are already wired
            // in WeatherParticleSystem for a future finer-grained weather UI.
            int weatherKind = 0;
            auto weatherIntensity = phoenix::runtime::WeatherIntensity::Heavy;
            if (weatherMode == WeatherMode::Storm)
                weatherKind = 1;
            else if (weatherMode == WeatherMode::Snowstorm)
                weatherKind = 2;
            weatherParticleSystem.set_weather(weatherKind, weatherIntensity);
            weatherParticleSystem.update(deltaSeconds, cameraPositionWorld,
                cameraRightWorld, cameraUpWorld, cameraForwardWorld);

            // Sun/moon discs, their halos and stars live in the sky shader.
            // This preserves cloud occlusion and prevents the old billboard
            // systems from drawing a second astro/field.

            // Merge every source into the single-upload buffers
            // update_effect_particles() expects — it fully replaces the GPU
            // buffers each call, so all of them must go in together.
            static std::vector<phoenix::renderer::TerrainVertex> combinedVertices;
            static std::vector<std::uint32_t> combinedIndices;
            static std::vector<phoenix::renderer::EffectParticleInstance> combinedInstances;
            static std::vector<phoenix::renderer::EffectParticleBatch> combinedBatches;
            combinedVertices = effectParticleSystem.vertices();
            combinedIndices = effectParticleSystem.indices();
            combinedInstances = effectParticleSystem.instances();
            combinedBatches = effectParticleSystem.batches();

            const auto append_source = [&](const std::vector<phoenix::renderer::TerrainVertex>& srcVertices,
                const std::vector<std::uint32_t>& srcIndices,
                const std::vector<phoenix::renderer::EffectParticleInstance>& srcInstances,
                const std::vector<phoenix::renderer::EffectParticleBatch>& srcBatches) {
                const auto vertexBase = static_cast<std::uint32_t>(combinedVertices.size());
                const auto instanceBase = static_cast<std::uint32_t>(combinedInstances.size());
                const auto indexBase = static_cast<std::uint32_t>(combinedIndices.size());

                combinedVertices.insert(combinedVertices.end(), srcVertices.begin(), srcVertices.end());
                for (const auto index : srcIndices)
                    combinedIndices.push_back(vertexBase + index);
                combinedInstances.insert(combinedInstances.end(), srcInstances.begin(), srcInstances.end());
                for (auto batch : srcBatches)
                {
                    batch.firstIndex += indexBase;
                    batch.firstInstance += instanceBase;
                    combinedBatches.push_back(batch);
                }
            };

            if (weatherParticleSystem.has_particles())
                append_source(weatherParticleSystem.vertices(), weatherParticleSystem.indices(),
                    weatherParticleSystem.instances(), weatherParticleSystem.batches());
            renderer.update_effect_particles(combinedVertices, combinedIndices, combinedInstances, combinedBatches);
        }


        if (uiAvailable)
        {
            // Lazily load (and cache) whichever data/effects file is currently
            // selected in the debug panel, rebuilding its sequence-name list.
            if (!effectLibraryFilePaths.empty() && selectedEffectFileIndex != loadedEffectFileIndex)
            {
                const auto fileIndex = static_cast<std::size_t>(
                    std::clamp(selectedEffectFileIndex, 0, static_cast<int>(effectLibraryFilePaths.size()) - 1));
                const auto& path = effectLibraryFilePaths[fileIndex];
                const auto& entry = ensureEffectLibraryLoaded(path);

                effectSequenceNames.clear();
                const auto& sequences = entry.library.sequences;
                effectSequenceNames.reserve(sequences.size());
                for (std::size_t i = 0; i < sequences.size(); ++i)
                {
                    // .EFT sequence names are stored in an unconverted Korean
                    // codepage, not UTF-8 — feeding the raw bytes to the UI
                    // renders as garbage "?" glyphs and isn't guaranteed to be
                    // valid UTF-8 at all. Reduce to printable ASCII only.
                    std::string label = std::to_string(i) + ": ";
                    for (const char raw : sequences[i].name)
                    {
                        const auto ch = static_cast<unsigned char>(raw);
                        label += (ch >= 0x20 && ch < 0x7F) ? static_cast<char>(ch) : '.';
                    }
                    if (label.size() == std::to_string(i).size() + 2)
                        label += "(unnamed)";
                    effectSequenceNames.push_back(std::move(label));
                }
                selectedEffectSequenceIndex = 0;
                loadedEffectFileIndex = selectedEffectFileIndex;
            }

            const auto panelResult = draw_editor_panel(
                runtime,
                renderer,
                fogEnabled,
                graphicsSettings.worldShadows,
                graphicsSettings.terrainCompatibility,
                graphicsSettings.vsyncEnabled,
                perfHud.fpsCapIndex,
                perfHud.antialiasingEnabled,
                perfHud.antialiasingAvailable,
                playMapSounds,
                playMapMusic,
                masterVolume,
                selectedMapIndex,
                viewDistance,
                weatherMode,
                characterOptions,
                selectedCharacterOption,
                characterAppearance,
                characterSystem,
                playableMode && characterLoaded && characterSystem.ready(),
                botManager.bots.size(),
                botManager.viewDistance,
                botManager.effectsEnabled,
                npcManager.catalog(),
                npcManager.active_count(),
                npcManager.status(),
                npcViewDistance,
                monsterManager.catalog(),
                monsterManager.active_count(),
                monsterManager.status(),
                monsterViewDistance,
                effectFileNames,
                selectedEffectFileIndex,
                effectSequenceNames,
                selectedEffectSequenceIndex,
                effectParticleSystem.debug_effect_count(),
                effectSpawnYOffset,
                effectComponentIndex,
                /*assetsReady=*/true);

            if (panelResult.loadRequested)
                pendingMapLoad = static_cast<std::size_t>(std::max(0, selectedMapIndex));
            else if (panelResult.viewDistanceChanged)
            {
                applyFogSettings();
                forceVisibilityUpdate = true;
            }
            else if (panelResult.weatherChanged)
            {
                applyFogSettings();
            }
            if (panelResult.terrainCompatibilityChanged)
                forceVisibilityUpdate = true;

            if (panelResult.characterChanged)
                reloadCharacterIntoRenderer();
            if (panelResult.emoteTriggered > 0)
                pendingEmote = panelResult.emoteTriggered;
            if (panelResult.animationTriggered > 0)
                pendingAnimation = panelResult.animationTriggered;
            if (panelResult.botSpawnCount > 0 && playableMode && characterLoaded && characterSystem.ready())
            {
                if (botManager.presetsBuilt)
                {
                    npcManager.clear(renderer);
                    botManager.spawn(panelResult.botSpawnCount,
                        characterSystem.world_x(), characterSystem.world_z(),
                        character_height_sampler, &heightSamplerCtx);
                }
                else
                {
                    pendingBotSpawnCount += panelResult.botSpawnCount;
                    if (!botPresetBuild)
                    {
                        const auto firstSlot = static_cast<std::uint32_t>(botTextureBaseSlot);
                        const auto textureWidth = renderer.terrain_texture_width();
                        const auto textureHeight = renderer.terrain_texture_height();
                        const auto textureMips = renderer.terrain_texture_mip_levels();
                        botPresetBuild.emplace(cpuLoader.submit(
                            [&botManager, &runtime, &characterOptions, &botEquipmentPools,
                                firstSlot, textureWidth, textureHeight, textureMips]() {
                                std::vector<phoenix::renderer::DdsTexture> slots(
                                    static_cast<std::size_t>(firstSlot) + kBotTextureSlotReserve);
                                if (!botManager.build_random_presets(
                                    runtime.state().assets.root,
                                    characterOptions,
                                    botEquipmentPools,
                                    firstSlot,
                                    kBotTextureSlotReserve,
                                    slots,
                                    /*allowPreload=*/false))
                                    return std::vector<phoenix::renderer::DdsTexture>{};

                                std::vector<phoenix::renderer::DdsTexture> textures(kBotTextureSlotReserve);
                                for (std::size_t i = 0; i < textures.size(); ++i)
                                {
                                    textures[i] = std::move(slots[static_cast<std::size_t>(firstSlot) + i]);
                                    auto& texture = textures[i];
                                    if (!texture.valid || textureWidth == 0
                                        || textureHeight == 0 || textureMips == 0)
                                        continue;
                                    const bool canonical = texture.compressed
                                        && texture.vkFormat == phoenix::renderer::kDdsFormatBc3UnormBlock
                                        && texture.width == textureWidth
                                        && texture.height == textureHeight
                                        && texture.mipData.size() >= textureMips;
                                    if (!canonical)
                                        phoenix::renderer::convert_texture_to_bc3(
                                            texture, textureWidth, textureHeight, textureMips);
                                }
                                return textures;
                            }));
                    }
                }
            }
            if (panelResult.clearBots)
            {
                pendingBotSpawnCount = 0;
                if (!botPresetBuild)
                    botManager.clear();
                renderer.set_bot_character_visible(false);
                reloadCharacterIntoRenderer();
                phoenix::app::release_memory_to_os();
            }
            // Random scatter around the player for bulk mob/NPC spawns. Disk
            // around the character (uniform area), each entity facing a random
            // way — like the bots' spread, but a smaller radius.
            static std::mt19937 spawnRng{ 0xC0FFEEu };
            const auto spawnPoint = [&](float maxRadius, float& sx, float& sy, float& sz, float& yaw) {
                std::uniform_real_distribution<float> u01(0.0f, 1.0f);
                const float angle = u01(spawnRng) * 6.2831853f;
                const float radius = 2.0f + (maxRadius - 2.0f) * std::sqrt(u01(spawnRng));
                sx = characterSystem.world_x() + std::sin(angle) * radius;
                sz = characterSystem.world_z() + std::cos(angle) * radius;
                sy = character_height_sampler(sx, sz, &heightSamplerCtx);
                yaw = u01(spawnRng) * 6.2831853f;
            };

            if (panelResult.npcSpawnCatalogIndex >= 0 || panelResult.npcSpawnRandom)
            {
                botManager.clear_bots();
                const int spawnCount = std::max(1, panelResult.npcSpawnCount);
                const auto& npcCat = npcManager.catalog();
                for (int n = 0; n < spawnCount && !npcCat.empty(); ++n)
                {
                    float sx, sy, sz, yaw;
                    spawnPoint(12.0f, sx, sy, sz, yaw);
                    const std::size_t cat = panelResult.npcSpawnRandom
                        ? static_cast<std::size_t>(spawnRng() % npcCat.size())
                        : static_cast<std::size_t>(panelResult.npcSpawnCatalogIndex);
                    npcManager.spawn(
                        runtime.state().assets.root, cat, sx, sy, sz, yaw,
                        static_cast<std::uint32_t>(npcTextureBaseSlot),
                        static_cast<std::uint32_t>(kNpcTextureSlotReserve),
                        renderer);
                }
            }
            if (panelResult.clearNpcs)
                npcManager.clear_manual(renderer);   // keep the map's .svmap NPCs
            if (panelResult.monsterSpawnCatalogIndex >= 0 || panelResult.monsterSpawnRandom)
            {
                const int spawnCount = std::max(1, panelResult.monsterSpawnCount);
                const auto& monCat = monsterManager.catalog();
                for (int n = 0; n < spawnCount && !monCat.empty(); ++n)
                {
                    float sx, sy, sz, yaw;
                    spawnPoint(14.0f, sx, sy, sz, yaw);
                    const std::size_t cat = panelResult.monsterSpawnRandom
                        ? static_cast<std::size_t>(spawnRng() % monCat.size())
                        : static_cast<std::size_t>(panelResult.monsterSpawnCatalogIndex);
                    monsterManager.spawn(
                        runtime.state().assets.root, cat, sx, sy, sz, yaw,
                        static_cast<std::uint32_t>(monsterTextureBaseSlot),
                        static_cast<std::uint32_t>(kMonsterTextureSlotReserve),
                        renderer);
                }
            }
            if (panelResult.clearMonsters)
                monsterManager.clear_manual(renderer);   // keep the map's .svmap mobs

            if (panelResult.effectSpawnRequested && loadedEffectFileIndex == selectedEffectFileIndex)
            {
                const auto& path = effectLibraryFilePaths[static_cast<std::size_t>(selectedEffectFileIndex)];
                const auto cacheIt = effectLibraryCache.find(path.string());
                if (cacheIt != effectLibraryCache.end())
                {
                    // Where the character actually stands, not the (offset,
                    // over-the-shoulder) third-person camera position.
                    float standX = cameraX, standY = cameraY, standZ = cameraZ, standYaw = cameraYaw;
                    if (playableMode && characterLoaded && characterSystem.ready())
                    {
                        standX = characterSystem.world_x();
                        standY = characterSystem.world_y();
                        standZ = characterSystem.world_z();
                        standYaw = characterSystem.world_yaw();
                    }
                    const float spawnPosition[3]{ standX, standY + effectSpawnYOffset, standZ };
                    effectParticleSystem.spawn_debug_effect(
                        cacheIt->second.library, cacheIt->second.textureLayers, cacheIt->second.meshes,
                        selectedEffectSequenceIndex, spawnPosition, standYaw, panelResult.effectSpawnOneShot);
                }
            }
            if (panelResult.effectComponentSpawnRequested && loadedEffectFileIndex == selectedEffectFileIndex)
            {
                const auto& path = effectLibraryFilePaths[static_cast<std::size_t>(selectedEffectFileIndex)];
                const auto cacheIt = effectLibraryCache.find(path.string());
                if (cacheIt != effectLibraryCache.end())
                {
                    float standX = cameraX, standY = cameraY, standZ = cameraZ, standYaw = cameraYaw;
                    if (playableMode && characterLoaded && characterSystem.ready())
                    {
                        standX = characterSystem.world_x();
                        standY = characterSystem.world_y();
                        standZ = characterSystem.world_z();
                        standYaw = characterSystem.world_yaw();
                    }
                    const float spawnPosition[3]{ standX, standY + effectSpawnYOffset, standZ };
                    effectParticleSystem.spawn_debug_component(
                        cacheIt->second.library, cacheIt->second.textureLayers, cacheIt->second.meshes,
                        effectComponentIndex, spawnPosition, standYaw, false);
                }
            }
            if (panelResult.clearEffects)
                effectParticleSystem.clear_debug_effects();

            draw_weather_overlay(
                weatherMode,
                currentView,
                totalTime,
                static_cast<float>(renderer.surface_width()),
                static_cast<float>(renderer.surface_height()));

            // Performance HUD.
            perfHud.worldX = cameraX;
            perfHud.worldY = cameraY;
            perfHud.worldZ = cameraZ;
            {
                const auto& mapNames = runtime.world_map_names();
                const auto mapIndex = runtime.selected_world_map();
                perfHud.mapId = (mapIndex < mapNames.size()) ? mapNames[mapIndex] : "";
            }
            draw_perf_hud(perfHud, static_cast<float>(renderer.surface_width()));
        }

        if (audioAvailable)
        {
            audioSystem.set_master_volume(masterVolume);
            float listenerX = cameraX;
            float listenerY = cameraY;
            float listenerZ = cameraZ;
            if (playableMode && characterLoaded && characterSystem.ready())
            {
                listenerX = characterSystem.world_x();
                listenerY = characterSystem.world_y();
                listenerZ = characterSystem.world_z();
            }

            // Which zones/emitters are audible only needs to be re-evaluated a few
            // times a second (sound sources don't move and the listener moves
            // continuously but smoothly) — recomputing distances against every
            // emitter and re-sorting candidates every single frame was pure
            // wasted work between these checks. audioSystem.update() itself still
            // runs every frame so volume fades stay smooth.
            static std::vector<phoenix::audio::AudibleTrack> audibleTracks;
            static float audioSceneTimer = 1.0f; // force an immediate first evaluation
            audioSceneTimer += deltaSeconds;
            if (audioSceneTimer >= 0.15f)
            {
                audioSceneTimer = 0.0f;
                audibleTracks.clear();
                build_audible_tracks_into(audibleTracks,
                    mapAudioScene,
                    listenerX,
                    listenerY,
                    listenerZ,
                    playMapMusic,
                    playMapSounds);
            }
            audioSystem.update(deltaSeconds, audibleTracks);

            // Footstep sounds based on terrain layer.
            if (playableMode && characterLoaded && characterSystem.ready())
            {
                static float footstepTimer = 0.0f;
                static float prevCharX = 0.0f;
                static float prevCharZ = 0.0f;

                const float cx = characterSystem.world_x();
                const float cz = characterSystem.world_z();
                const float dx = cx - prevCharX;
                const float dz = cz - prevCharZ;
                const float distSq = dx * dx + dz * dz;
                prevCharX = cx;
                prevCharZ = cz;

                const bool moving = distSq > 0.001f;
                const bool fast = distSq > 1.0f;

                // Footsteps only while actually on the ground — never while
                // jumping/falling, swimming or climbing a ladder.
                const bool onGround = characterSystem.grounded()
                    && !characterSystem.swimming()
                    && !characterSystem.climbing();

                if (moving && onGround)
                {
                    footstepTimer += deltaSeconds;
                    const float interval = fast ? 0.30f : 0.45f;
                    if (footstepTimer >= interval)
                    {
                        footstepTimer = 0.0f;
                        const auto soundPath = runtime.walk_sound_at(cx, cz);
                        if (!soundPath.empty())
                            audioSystem.play_once(soundPath, 0.35f);
                    }
                }
                else
                {
                    footstepTimer = 0.0f;
                }
            }
        }

        // In-world labels are authored by the game and rendered by the engine's
        // native text pass; the debug UI uses the same tiny atlas renderer.
        std::vector<phoenix::renderer::ScreenLabel> worldLabels;
        if (npcManager.active() || monsterManager.active())
        {
            const float w = static_cast<float>(std::max(1u, renderer.surface_width()));
            const float h = static_cast<float>(std::max(1u, renderer.surface_height()));
            constexpr float labelFontSize = 14.0f;
            // Fixed screen gap between the NPC's head and the label: keeping this in
            // pixels (not world units) makes the label sit at a consistent spot
            // regardless of camera distance, instead of drifting up when close.
            constexpr float labelPixelGap = 5.0f;
            // Names show closer than the NPC render cull to avoid distant clutter.
            constexpr float labelMaxDistance = 50.0f;
            const float labelMaxDistSq = labelMaxDistance * labelMaxDistance;
            constexpr std::uint8_t nameCol[4]{ 255, 226, 0, 255 };
            constexpr std::uint8_t typeCol[4]{ 90, 200, 255, 255 };
            const auto drawCentered = [&](float cx, float top, const char* text, const std::uint8_t (&color)[4]) {
                phoenix::renderer::ScreenLabel output{};
                output.centerX = cx;
                output.topY = top;
                output.text = text;
                std::copy(std::begin(color), std::end(color), std::begin(output.color));
                worldLabels.push_back(std::move(output));
            };
            // Case-insensitive match for the generic types whose label is hidden.
            const auto iequals = [](const std::string& a, const char* b) {
                const auto lc = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c + 32) : c; };
                std::size_t i = 0;
                for (; i < a.size() && b[i]; ++i)
                    if (lc(a[i]) != lc(b[i]))
                        return false;
                return i == a.size() && b[i] == '\0';
            };
            for (const auto& label : npcManager.labels())
            {
                const float ddx = label.x - currentView.x;
                const float ddy = label.y - currentView.y;
                const float ddz = label.z - currentView.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz > labelMaxDistSq)
                    continue;
                // Hidden behind world geometry (buildings/assets): don't draw.
                const float eye[3] = { currentView.x, currentView.y, currentView.z };
                const float anchor[3] = { label.x, label.y, label.z };
                if (worldCollisionMesh.segment_occluded(eye, anchor))
                    continue;
                phoenix::renderer::ScreenPoint sp{};
                if (!project_world_to_screen(currentView, label.x, label.y, label.z, w, h, sp))
                    continue;
                // sp is the NPC's head; stack the text upward from a fixed pixel
                // gap above it. Name is the bottom line (just above the head);
                // type sits above it. 'Normal'/'Animal' types are hidden.
                const float lineHeight = labelFontSize + 1.0f;
                const float nameTop = sp.y - labelPixelGap - labelFontSize;
                const bool showType = label.typeName && !label.typeName->empty()
                    && !iequals(*label.typeName, "Normal") && !iequals(*label.typeName, "Animal")
                    && !iequals(*label.typeName, "DeadNpc") && !iequals(*label.typeName, "GamblingHouse");
                if (showType)
                    drawCentered(sp.x, nameTop - lineHeight, label.typeName->c_str(), typeCol);
                if (label.name && !label.name->empty())
                    drawCentered(sp.x, nameTop, label.name->c_str(), nameCol);
            }
            // Monster labels: name only (no type).
            for (const auto& label : monsterManager.labels())
            {
                const float ddx = label.x - currentView.x;
                const float ddy = label.y - currentView.y;
                const float ddz = label.z - currentView.z;
                if (ddx * ddx + ddy * ddy + ddz * ddz > labelMaxDistSq)
                    continue;
                const float eye[3] = { currentView.x, currentView.y, currentView.z };
                const float anchor[3] = { label.x, label.y, label.z };
                if (worldCollisionMesh.segment_occluded(eye, anchor))
                    continue;
                phoenix::renderer::ScreenPoint sp{};
                if (!project_world_to_screen(currentView, label.x, label.y, label.z, w, h, sp))
                    continue;
                phoenix::renderer::ScreenPoint feet{};
                if (!project_world_to_screen(currentView, label.x, label.baseY, label.z, w, h, feet))
                    continue;

                // Mobs expose their name only while the pointer is over the
                // projected creature, matching the Godot client and avoiding a
                // permanent field of labels. Derive the hit area from the actual
                // head/feet projection so it remains stable at every distance.
                const auto [mouseX, mouseY] = window.mouse_position();
                const float bodyHeight = std::clamp(std::abs(feet.y - sp.y), 10.0f, 180.0f);
                const float halfWidth = std::clamp(bodyHeight * 0.34f, 7.0f, 55.0f);
                const float top = std::min(sp.y, feet.y) - 4.0f;
                const float bottom = std::max(sp.y, feet.y) + 5.0f;
                const bool hovered = !uiWantsMouse
                    && static_cast<float>(mouseX) >= sp.x - halfWidth && static_cast<float>(mouseX) <= sp.x + halfWidth
                    && static_cast<float>(mouseY) >= top && static_cast<float>(mouseY) <= bottom;
                if (!hovered)
                    continue;
                if (label.name && !label.name->empty())
                    drawCentered(sp.x, sp.y - labelPixelGap - labelFontSize, label.name->c_str(), nameCol);
            }
        }

        renderer.set_world_labels(std::move(worldLabels));
        renderer.set_screen_ui(phoenix::ui::px::end_frame());

        renderer.render_frame();

        // FPS cap.
        {
            const float capSeconds = perfHud.fps_cap_seconds();
            if (capSeconds > 0.0f)
            {
                while (std::chrono::duration<float>(clock::now() - lastFrame).count() < capSeconds)
                    ;
            }
        }
        if (pendingMapLoad)
        {
            const auto mapIdx = *pendingMapLoad;
            pendingMapLoad.reset();

            // Put a finished loading frame on screen before releasing the old
            // world. No further old-world draw is submitted after this point.
            renderer.enter_loading_mode();
            renderer.set_world_labels({});
            renderer.set_screen_ui({});
            showLoading(0.02f, "Changing map");
            audioSystem.stop_all();
            if (botPresetBuild)
            {
                botPresetBuild->wait();
                botPresetBuild.reset();
            }
            pendingBotSpawnCount = 0;
            botManager.clear();
            npcManager.clear(renderer);
            monsterManager.clear(renderer);

            // Release the old map before decoding the replacement. This keeps
            // maps from overlapping in CPU/GPU memory and covers resource
            // kinds which the next map does not use at all.
            effectParticleSystem.release_map_resources();
            weatherParticleSystem.release_map_resources();
            std::vector<PendingBotEffect>().swap(pendingBotEffects);
            characterSystem.set_ladders({});
            mapAudioScene = {};
            staticObjectScene = {};
            animatedObjectScene = {};
            worldCollisionMesh = {};
            terrainLodInfo = {};
            std::vector<phoenix::renderer::TerrainDrawRange>().swap(visibleTerrainRanges);
            std::vector<phoenix::renderer::ObjectBatch>().swap(visibleBatches);
            std::vector<phoenix::renderer::ObjectBatch>().swap(visibleAnimatedBatches);
            lastCullView = {};
            terrainTexturesUploaded = false;
            terrainVertexCount = 0;
            terrainIndexCount = 0;
            objectInstanceCount = 0;
            objectBatchCount = 0;
            if (streamedSceneBuild)
            {
                streamedSceneBuild->wait();
                streamedSceneBuild.reset();
            }
            if (streamedTerrainBuild)
            {
                streamedTerrainBuild->wait();
                streamedTerrainBuild.reset();
            }
            worldStreamer.reset();
            runtime.release_world_map_resources();
            renderer.release_world_resources();
            phoenix::app::release_memory_to_os();

            showLoading(0.05f, "Changing map");
            if (runAsync([&]() { return runtime.load_world_map(mapIdx); }, 0.10f, "Loading world"))
            {
                uploadCurrentWorld();
                applyFogSettings();
            }
            lastFrame = clock::now();
        }

    }

    // Immediate exit: hide the window so the close feels instant, stop audio
    // cleanly (avoids an output pop), persist the HUD settings, and let the OS
    // reclaim everything else. Tearing down gigabytes of caches, joining the
    // preload thread and waiting on the GPU only delays the close.
    window.hide();
    audioSystem.shutdown();
    phoenix::ui::flush_app_settings();
    std::_Exit(0);
}
