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
#include "character/weapon_effect.h"
#include "effects/effect_system.h"
#include "effects/effect_placement.h"
#include "runtime/playable_spawn.h"
#include "runtime/phoenix_runtime.h"
#include "platform/sdl_window.h"
#include "renderer/dds_loader.h"
#include "renderer/visibility_culling.h"
#include "renderer/vulkan_renderer.h"
#include "ui/editor_panel.h"
#include "ui/loading_screen.h"
#include "ui/perf_hud.h"
#include "ui/weather_overlay.h"
#include "world/portal_runtime.h"

#include "imgui.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace
{
    // UI types/functions live in ui/editor_panel.{h,cpp} and ui/perf_hud.{h,cpp}.
    using phoenix::ui::WeatherMode;
    using phoenix::ui::WaterMode;
    using phoenix::ui::CharacterOption;
    using phoenix::ui::UnifiedPanelResult;
    using phoenix::ui::apply_renderer_fog;
    using phoenix::ui::apply_renderer_water_style;
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
    using phoenix::renderer::extract_gpu_bounds;
    using phoenix::renderer::project_world_to_screen;
    using phoenix::renderer::sort_scene_front_to_back;
    using phoenix::renderer::sphere_visible;
    using phoenix::character::BotManager;
    using phoenix::character::scan_bot_equipment_pools;
    using phoenix::character::scan_character_options;
    using phoenix::world::PendingTeleport;
    using phoenix::world::check_portal_activation;
    using phoenix::effects::place_portal_effects;

    constexpr const char* kAppTitle = "Phoenix Engine";
    constexpr std::size_t kSkyTextureLayer = 63;
    constexpr std::size_t kPrimaryCloudTextureLayer = 64;
    constexpr std::size_t kSecondaryCloudTextureLayer = 65;
    constexpr std::size_t kAssetTextureLayerBase = 66;

}

int main(int, char**)
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;

    const auto executableDir = phoenix::app::executable_directory();

    phoenix::platform::SdlWindow window;
    if (!window.create(kWidth, kHeight, kAppTitle))
    {
        std::fprintf(stderr, "Could not create Phoenix Engine window.\n");
        return 1;
    }

    const auto [clientWidth, clientHeight] = window.client_size();
    phoenix::renderer::VulkanRenderer renderer;
    if (!renderer.initialize(
        window.handle(),
        static_cast<std::uint32_t>(clientWidth > 0 ? clientWidth : kWidth),
        static_cast<std::uint32_t>(clientHeight > 0 ? clientHeight : kHeight)))
    {
        std::fprintf(stderr, "Could not initialize Vulkan.\n");
        return 1;
    }

    const auto imguiAvailable = renderer.initialize_imgui(window.handle());

    phoenix::app::LoadingScheduler cpuLoader(phoenix::app::LoadingWorkKind::Cpu);
    phoenix::app::LoadingScheduler ioLoader(phoenix::app::LoadingWorkKind::Io);
    PerfHudState perfHud;
    perfHud.initialize_system_info();
    perfHud.gpuName = renderer.adapter_name();
    perfHud.renderer = &renderer;
    perfHud.load_settings(executableDir);

    auto showLoading = [&](float /*progress*/, std::string_view /*stage*/) {
        window.pump_messages();
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
    showLoading(0.12f, "Indexing data");

    // Character system. Its catalog preload runs after the world load (each phase
    // is internally multithreaded, so they get full CPU cores rather than fighting
    // each other if overlapped).
    phoenix::character::CharacterSystem characterSystem;
    phoenix::character::WeaponEffect weaponEffect;
    phoenix::effects::EffectManager effectManager;
    phoenix::renderer::ParticleBatch particleBatch;
    std::vector<phoenix::renderer::ParticleInstance> particleScratch;
    phoenix::character::CharacterAppearance characterAppearance{};
    BotManager botManager;

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
    // The full character/item caches (all races + BC3 textures, ~93MB, several
    // seconds) used to be built synchronously here. To launch fast we now skip that:
    // the default character (Humf on map 1) loads via on-demand disk fallback, and
    // the heavy preload runs in a background thread after the world is ready (see
    // backgroundAssetThread below). assetsFullyLoaded gates the appearance UI until
    // the caches exist, so appearance swaps remain instant and race-free.
    std::atomic<bool> assetsFullyLoaded{ false };
    std::thread backgroundAssetThread;
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
    bool showCollisionDebug = false;
    WeatherMode weatherMode = WeatherMode::Default;
    WaterMode waterMode = WaterMode::Natural;
    // fogCullDistance is the actual cull boundary: nothing beyond the fog-end is
    // visible, so nothing beyond it should be rendered.
    float fogCullDistance = viewDistance;
    const auto applyFogSettings = [&]() {
        fogCullDistance = apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);
    };
    applyFogSettings();
    apply_renderer_water_style(renderer, waterMode);
    int pendingEmote = 0;   // emote triggered from ImGui, consumed next frame

    std::uint32_t terrainVertexCount{};
    std::uint32_t terrainIndexCount{};
    phoenix::runtime::PhoenixRuntime::TerrainLodInfo terrainLodInfo;
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
    phoenix::runtime::WorldCollisionMesh worldCollisionMesh;
    HeightSamplerContext heightSamplerCtx{ &runtime, &worldCollisionMesh };
    MapAudioScene mapAudioScene;
    bool forceVisibilityUpdate = true;
    std::optional<std::size_t> pendingMapLoad;
    // Portal teleport: walking into a portal queues a destination map load here and
    // remembers where to drop the character on arrival. A cooldown prevents
    // re-triggering a portal immediately after spawning on/near one.
    std::optional<PendingTeleport> pendingTeleportDestination;
    float portalCooldown = 0.0f;
    CameraView lastCullView{};

    const auto uploadDebugGizmos = [&]() {
        std::vector<phoenix::renderer::TerrainVertex> debugVertices;
        std::vector<std::uint32_t> debugIndices;

        if (showCollisionDebug && !worldCollisionMesh.triangles.empty())
        {
            for (const auto& tri : worldCollisionMesh.triangles)
            {
                const auto base = static_cast<std::uint32_t>(debugVertices.size());
                phoenix::renderer::TerrainVertex v{};
                v.color[0] = 0.0f; v.color[1] = 1.0f; v.color[2] = 0.3f;
                v.normal[1] = 1.0f;
                v.textureLayer = 0xFFFFFFFFu;

                v.position[0] = tri.v0[0]; v.position[1] = tri.v0[1]; v.position[2] = tri.v0[2];
                debugVertices.push_back(v);
                v.position[0] = tri.v1[0]; v.position[1] = tri.v1[1]; v.position[2] = tri.v1[2];
                debugVertices.push_back(v);
                v.position[0] = tri.v2[0]; v.position[1] = tri.v2[1]; v.position[2] = tri.v2[2];
                debugVertices.push_back(v);

                debugIndices.push_back(base + 0);
                debugIndices.push_back(base + 1);
                debugIndices.push_back(base + 2);
            }
        }

        renderer.set_debug_mesh(debugVertices, debugIndices);
        renderer.set_debug_visible(showCollisionDebug);
    };

    const auto uploadCharacterMesh = [&]() {
        if (!characterLoaded || !characterSystem.ready())
            return;
        phoenix::app::set_character_mesh(renderer, characterSystem, playableMode);
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
        float progressEnd = 0.0f) {
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

        // Compute mip count: down to 4Ã—4 blocks (same as renderer logic).
        const auto maxDim = std::max(targetW, targetH);
        const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
        const auto targetMips = std::min(fullMips,
            static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)));

        const auto workerCount = std::min(textures.size(), cpuLoader.worker_count());
        if (workerCount <= 1)
        {
            for (std::size_t idx = 0; idx < textures.size(); ++idx)
            {
                phoenix::renderer::convert_texture_to_bc3(textures[idx], targetW, targetH, targetMips);
                if (!loadingStage.empty() && (idx % 4u == 0u || idx + 1u == textures.size()))
                {
                    const float t = static_cast<float>(idx + 1u) / static_cast<float>(textures.size());
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
                    if (idx >= textures.size())
                        break;
                    phoenix::renderer::convert_texture_to_bc3(textures[idx], targetW, targetH, targetMips);
                    completedTex.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }));
        }

        while (completedTex.load(std::memory_order_relaxed) < textures.size())
        {
            if (!loadingStage.empty())
            {
                const float t = static_cast<float>(completedTex.load(std::memory_order_relaxed)) / static_cast<float>(textures.size());
                showLoading(progressStart + (progressEnd - progressStart) * t, loadingStage);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
        for (auto& future : futures)
            future.get();
    };

    constexpr std::size_t kCharacterTextureSlotReserve = 32;
    constexpr std::size_t kBotTextureSlotReserve = 256;

    const auto reloadCharacterIntoRenderer = [&]() {
        if (characterTextureBaseSlot == 0 || !terrainTexturesUploaded)
            return false;

        characterLoaded = characterSystem.load(
            runtime.state().assets.root, characterAppearance, assetsFullyLoaded.load());
        if (!characterLoaded)
            return false;

        characterSystem.set_height_sampler(character_height_sampler, &heightSamplerCtx);
        characterSystem.set_collision_callback(character_collision_callback, &worldCollisionMesh);
        const auto& charTexPaths = characterSystem.texture_paths();
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(characterTextureBaseSlot));

        // Fast path: lookup pre-cached BC3 textures (no disk I/O, no conversion).
        if (characterSystem.bc3_cache_ready() && charTexPaths.size() <= kCharacterTextureSlotReserve)
        {
            std::vector<phoenix::renderer::DdsTexture> characterTextures(charTexPaths.size());
            bool allCached = true;
            for (std::size_t i = 0; i < charTexPaths.size(); ++i)
            {
                const auto* cached = characterSystem.bc3_texture_for(charTexPaths[i]);
                if (cached)
                    characterTextures[i] = *cached; // copy BC3 mip data from RAM cache
                else
                {
                    allCached = false;
                    break;
                }
            }
            if (!allCached)
            {
                // Fallback: load from disk + convert (should be rare).
                for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                    characterTextures[i] = phoenix::renderer::load_dds(charTexPaths[i]);
                normalizeTexturesForBcUpload(characterTextures);
            }
            renderer.upload_terrain_texture_layers(static_cast<std::uint32_t>(characterTextureBaseSlot), characterTextures);
        }
        else
        {
            // Fallback: load from disk + convert.
            std::vector<phoenix::renderer::DdsTexture> characterTextures(charTexPaths.size());
            for (std::size_t i = 0; i < charTexPaths.size(); ++i)
                characterTextures[i] = phoenix::renderer::load_dds(charTexPaths[i]);
            normalizeTexturesForBcUpload(characterTextures);
            renderer.upload_terrain_texture_layers(static_cast<std::uint32_t>(characterTextureBaseSlot), characterTextures);
        }

        // Fast mesh update: reuses existing GPU buffers, no vkDeviceWaitIdle.
        phoenix::app::update_character_mesh(renderer, characterSystem, playableMode);
        return true;
    };

    const auto uploadCurrentWorld = [&]() {
        renderer.enter_loading_mode();
        // Drop any looping effects (e.g. portals) carried over from the previous map.
        effectManager.clear();
        showLoading(0.36f, "Preparing scene");
        applyFogSettings();
        mapAudioScene = build_map_audio_scene(runtime);
        const auto texturePaths = runtime.terrain_texture_paths();
        const auto waterTexturePath = runtime.water_texture_path();
        const auto skyTexturePath = runtime.texture_path_for(runtime.state().world.skyFileName);
        const auto primaryCloudTexturePath = runtime.texture_path_for(runtime.state().world.primaryCloudFileName);
        const auto secondaryCloudTexturePath = runtime.texture_path_for(runtime.state().world.secondaryCloudFileName);
        const auto assetTexturePaths = runtime.asset_texture_paths();

        runtime.load_water_animation();
        const auto& waterAnim = runtime.water_animation();
        const auto waterLayout = phoenix::app::make_water_resource_layout(
            kAssetTextureLayerBase,
            assetTexturePaths.size(),
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
            for (std::size_t i = 0; i < texturePaths.size(); ++i)
            {
                const auto& tex = terrainTextures[i];
            }

            std::size_t loadedAssetTextures = 0;
            std::size_t failedAssetTextures = 0;
            for (std::size_t i = 0; i < assetTexturePaths.size(); ++i)
            {
                const auto& tex = terrainTextures[kAssetTextureLayerBase + i];
                if (tex.valid)
                    ++loadedAssetTextures;
                else
                    ++failedAssetTextures;

                {
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
            const auto botTextureBaseSlot = characterTextureBaseSlot + kCharacterTextureSlotReserve;
            const auto reservedEnd = botTextureBaseSlot + kBotTextureSlotReserve;
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
                        runtime.state().assets.root, characterAppearance, assetsFullyLoaded.load());
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

            if (characterLoaded)
            {
                const bool botPresetsReady = runAsync([&]() {
                    return botManager.build_random_presets(
                        runtime.state().assets.root,
                        characterOptions,
                        botEquipmentPools,
                        static_cast<std::uint32_t>(botTextureBaseSlot),
                        kBotTextureSlotReserve,
                        terrainTextures,
                        assetsFullyLoaded.load());
                }, 0.67f, "Preparing bot presets");
            }
        }

        showLoading(0.68f, "Uploading textures");
        if (!terrainTextures.empty())
        {
            // Log pre-normalisation format census.
            {
                std::uint32_t countBc1{}, countBc3{}, countRgba{}, countInvalid{};
                std::map<std::string, std::uint32_t> sizeDistribution;
                for (const auto& t : terrainTextures)
                {
                    if (!t.valid) { ++countInvalid; continue; }
                    auto sizeKey = std::to_string(t.width) + "x" + std::to_string(t.height);
                    sizeDistribution[sizeKey]++;
                    if (t.compressed && t.vkFormat == VK_FORMAT_BC1_RGBA_UNORM_BLOCK) ++countBc1;
                    else if (t.compressed && (t.vkFormat == VK_FORMAT_BC3_UNORM_BLOCK || t.vkFormat == VK_FORMAT_BC2_UNORM_BLOCK)) ++countBc3;
                    else ++countRgba;
                }
            }

            showLoading(0.70f, "Normalising textures to BC3");
            normalizeTexturesForBcUpload(terrainTextures, "Normalising textures to BC3", 0.70f, 0.735f);

            showLoading(0.74f, "Uploading BC3 textures");
            terrainTexturesUploaded = renderer.upload_terrain_textures(terrainTextures);
            if (terrainTexturesUploaded)
            {
                releaseDecodedTextureRam(terrainTextures);
                // Also release mip data â€” GPU has it now.
                for (auto& t : terrainTextures)
                    std::vector<std::vector<std::uint8_t>>().swap(t.mipData);
            }
        }

        const auto& world = runtime.state().world;
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
                static_cast<std::uint32_t>(tileSizes.size()));
        }

        // Load and upload field lightmaps (baked terrain shadows).
        {
            std::uint32_t lmSectionCount = 0;
            const auto lmPaths = runtime.field_lightmap_paths(lmSectionCount);
            if (!lmPaths.empty() && lmSectionCount > 0)
            {
                std::vector<phoenix::renderer::DdsTexture> lightmaps;
                for (const auto& p : lmPaths)
                {
                    if (!p.empty() && std::filesystem::exists(p))
                        lightmaps.push_back(phoenix::renderer::load_dds(p));
                    else
                        lightmaps.push_back({}); // invalid placeholder
                }
                if (renderer.upload_field_lightmaps(lightmaps, lmSectionCount))
                {
                }
            }
        }

        std::vector<phoenix::renderer::TerrainVertex> terrainVertices;
        std::vector<std::uint32_t> terrainIndices;
        std::vector<phoenix::renderer::TerrainVertex> waterVertices;
        std::vector<std::uint32_t> waterIndices;

        runAsyncVoid([&]() { runtime.build_terrain_mesh(terrainVertices, terrainIndices, terrainLodInfo); }, 0.74f, "Building terrain");
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

        // Free terrain mesh CPU data â€” now in GPU buffers.
        { std::vector<phoenix::renderer::TerrainVertex>().swap(terrainVertices);
          std::vector<std::uint32_t>().swap(terrainIndices);
          std::vector<phoenix::renderer::TerrainVertex>().swap(waterVertices);
          std::vector<std::uint32_t>().swap(waterIndices); }

        staticObjectScene = runAsync([&runtime]() {
            return runtime.build_static_object_scene();
        }, 0.82f, "Building static objects");
        animatedObjectScene = runAsync([&runtime]() {
            return runtime.build_animated_object_scene();
        }, 0.87f, "Building animated objects");
        objectInstanceCount = static_cast<std::uint32_t>(staticObjectScene.instances.size());
        objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
        {
        }
        if (!renderer.set_static_object_mesh(
            staticObjectScene.vertices,
            staticObjectScene.indices,
            staticObjectScene.instances,
            staticObjectScene.batches))

        // Upload indirect draw data for GPU frustum culling.
        if (renderer.upload_indirect_draw_data(staticObjectScene.batches, extract_gpu_bounds(staticObjectScene)))

        // Upload animated object mesh to GPU.
        if (!animatedObjectScene.vertices.empty())
        {
            renderer.set_animated_object_mesh(
                animatedObjectScene.vertices,
                animatedObjectScene.indices,
                animatedObjectScene.instances,
                animatedObjectScene.batches);
        }

        showLoading(0.90f, "Finalizing scene");
        uploadDebugGizmos();

        worldCollisionMesh = runAsync([&]() { return runtime.build_collision_mesh(); }, 0.92f, "Building collision");

        // ---- Release CPU-side data already uploaded to GPU ----
        // Vertices and indices are now in GPU buffers; free CPU copies.
        {
            std::size_t freedMB = 0;
            const auto countBytes = [](auto& vec) {
                const auto bytes = vec.capacity() * sizeof(typename std::remove_reference_t<decltype(vec)>::value_type);
                vec.clear();
                vec.shrink_to_fit();
                return bytes;
            };
            freedMB += countBytes(staticObjectScene.vertices);
            freedMB += countBytes(staticObjectScene.indices);
        }

        // Upload character mesh (initial bind pose).
        if (characterLoaded && characterSystem.ready())
            uploadCharacterMesh();

        // ---- Spawn the fiery Portal effect at every gate on this map ----
        {
            const auto portalCount = place_portal_effects(effectManager, runtime);
        }
        // ---- Initial playable spawn: centre of the map (valid interior point in
        // dungeons), snapped to a walkable surface. A queued portal teleport, if
        // any, overrides this afterwards in the pendingMapLoad handler.
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

        forceVisibilityUpdate = true;
        showLoading(1.0f, "Ready");

        phoenix::app::release_memory_to_os();
    };

    uploadCurrentWorld();
    applyFogSettings();
    window.set_title(kAppTitle);

    // World + default character are up and the window is interactive. Now build the
    // full character/item caches (all races + BC3 textures) off the main thread so
    // the rest of the content streams in without blocking play. assetsFullyLoaded is
    // flipped when done; the appearance UI stays disabled until then so there is no
    // concurrent reader of the caches while this thread writes them.
    backgroundAssetThread = std::thread([&]() {
        // Lower the thread priority so the background preload yields CPU to the
        // game loop. Before the deferred-loading change this work ran during the
        // loading screen (invisible spike); now it runs while the game is
        // interactive, so it must not compete with rendering.
        phoenix::app::set_current_thread_loading_priority();
        characterSystem.preload(runtime.state().assets.root);
        characterSystem.preload_items(runtime.state().assets.root);
        assetsFullyLoaded.store(true);
    });

    using clock = std::chrono::steady_clock;
    auto lastFrame = clock::now();
    float totalTime = 0.0f;
    bool playToggleWasDown = false;
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
            // and recreates the swapchain â€” otherwise the window stays frozen.
            lastClientSize = { 0, 0 };
            lastFrame = clock::now();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const bool restoreEvent = window.consume_restore_event();
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

        if (imguiAvailable)
            renderer.begin_imgui_frame();
        const auto imguiWantsKeyboard = imguiAvailable && ImGui::GetIO().WantCaptureKeyboard;
        const auto imguiWantsMouse = imguiAvailable && ImGui::GetIO().WantCaptureMouse;

        const auto [mouseDx, mouseDy] = window.consume_mouse_delta();
        const auto mouseWheel = window.consume_mouse_wheel_delta();

        float cameraX{};
        float cameraY{};
        float cameraZ{};
        float cameraYaw{};
        float cameraPitch{};

        if (playableMode && characterLoaded && characterSystem.ready())
        {
            // ---- Playable mode: third-person character control ----
            phoenix::character::PlayableInput pInput{};
            if (!imguiWantsKeyboard)
            {
                pInput.forward = window.is_key_down(SDLK_w);
                pInput.backward = window.is_key_down(SDLK_s);
                pInput.left = window.is_key_down(SDLK_a);
                pInput.right = window.is_key_down(SDLK_d);
                pInput.jump = window.is_key_down(SDLK_SPACE);
                pInput.fast = window.is_key_down(SDLK_LSHIFT);
                pInput.yawLeft = window.is_key_down(SDLK_LEFT);
                pInput.yawRight = window.is_key_down(SDLK_RIGHT);
                pInput.pitchUp = window.is_key_down(SDLK_UP);
                pInput.pitchDown = window.is_key_down(SDLK_DOWN);
                pInput.sit = window.is_key_down(SDLK_c);
            }
            pInput.cameraDrag = !imguiWantsMouse && window.is_mouse_button_down(1);
            pInput.mouseDx = !imguiWantsMouse ? static_cast<float>(mouseDx) : 0.0f;
            pInput.mouseDy = !imguiWantsMouse ? static_cast<float>(mouseDy) : 0.0f;
            pInput.mouseWheel = !imguiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

            // Apply pending emote from ImGui (set last frame's panel result).
            pInput.emote = pendingEmote;
            pendingEmote = 0;  // consumed

            heightSamplerCtx.lastCharacterY = characterSystem.world_y();
            characterSystem.update(deltaSeconds, pInput);
            characterSystem.camera_state(cameraX, cameraY, cameraZ, cameraYaw, cameraPitch);

            // ---- Portal teleport ----
            // Walking into a portal box queues a load of its destination map and
            // remembers the destination position. A short cooldown (after spawn and
            // after each teleport) avoids instant re-triggering on the arrival gate.
            if (portalCooldown > 0.0f)
                portalCooldown -= deltaSeconds;
            if (!pendingMapLoad && portalCooldown <= 0.0f)
            {
                const auto activation = check_portal_activation(
                    runtime,
                    runtime.world_map_names(),
                    characterSystem.world_x(),
                    characterSystem.world_y(),
                    characterSystem.world_z());
                if (activation)
                {
                    if (!activation->destinationMapIndex)
                    {
                        portalCooldown = 1.0f;
                    }
                    else
                    {
                        pendingTeleportDestination = activation->teleport;
                        pendingMapLoad = *activation->destinationMapIndex;
                        portalCooldown = 2.0f;
                        const auto& tp = activation->teleport;
                    }
                }
            }

            botManager.update(deltaSeconds, cameraX, cameraZ, fogCullDistance,
                character_height_sampler, &heightSamplerCtx);

            // Spawn one-shot effects queued by bots.
            if (!botManager.pendingEffects.empty())
            {
                const auto& catalog = phoenix::effects::preset_catalog();
                for (const auto& pe : botManager.pendingEffects)
                {
                    if (pe.catalogIndex < catalog.size())
                        effectManager.spawn(catalog[pe.catalogIndex],
                            phoenix::effects::EffectAnchor::at(pe.x, pe.y + 1.0f, pe.z));
                }
            }

            const auto& charVerts = characterSystem.world_vertices();
            const auto* tv = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(charVerts.data());

            renderer.update_character_vertices(tv, charVerts.size());
            if (botManager.bots.empty())
            {
                renderer.set_bot_character_visible(false);
            }
            else
            {
                if (botManager.updatePoseMesh(renderer))
                {
                    botManager.updatePoseInstances(renderer);
                    renderer.set_bot_character_visible(true);
                }
            }
        }
        else
        {
            // ---- Viewer mode: free camera ----
            phoenix::runtime::CameraInput cameraInput{};
            if (!imguiWantsKeyboard)
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
            cameraInput.look = !imguiWantsMouse && window.is_mouse_button_down(1);
            cameraInput.mouseDx = !imguiWantsMouse ? static_cast<float>(mouseDx) : 0.0f;
            cameraInput.mouseDy = !imguiWantsMouse ? static_cast<float>(mouseDy) : 0.0f;
            cameraInput.wheel = !imguiWantsMouse ? static_cast<float>(mouseWheel) : 0.0f;

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

        // Snap camera position to a fine grid to eliminate sub-pixel jitter
        // caused by continuous EMA convergence (gravity/terrain smoothing).
        // Grid = 1/512 world units, below a visible pixel at typical view distances.
        constexpr float kCameraSnap = 512.0f;
        const float snappedX = std::round(cameraX * kCameraSnap) / kCameraSnap;
        const float snappedY = std::round(cameraY * kCameraSnap) / kCameraSnap;
        const float snappedZ = std::round(cameraZ * kCameraSnap) / kCameraSnap;
        const float renderCameraX = snappedX;
        const float renderCameraY = snappedY;
        const float renderCameraZ = snappedZ;
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

        const auto cullMoveDx = currentView.x - lastCullView.x;
        const auto cullMoveDy = currentView.y - lastCullView.y;
        const auto cullMoveDz = currentView.z - lastCullView.z;
        const auto cullMoveSq = cullMoveDx * cullMoveDx + cullMoveDy * cullMoveDy + cullMoveDz * cullMoveDz;
        const bool visibilityNeedsUpdate = forceVisibilityUpdate
            || cullMoveSq > 96.0f * 96.0f
            || std::abs(currentView.yaw - lastCullView.yaw) > 0.08f
            || std::abs(currentView.pitch - lastCullView.pitch) > 0.06f
            || std::abs(currentView.distance - lastCullView.distance) > 32.0f
            || std::abs(currentView.aspect - lastCullView.aspect) > 0.01f;
        if (visibilityNeedsUpdate)
        {
            static std::vector<phoenix::renderer::TerrainDrawRange> visibleTerrainRanges;
            build_visible_terrain_ranges(visibleTerrainRanges, runtime, currentView, terrainLodInfo);
            renderer.set_terrain_draw_ranges(visibleTerrainRanges);
            std::uint32_t visibleTerrainIndexCount{};
            for (const auto& range : visibleTerrainRanges)
                visibleTerrainIndexCount += range.indexCount;
            if (visibleTerrainIndexCount > 0)
                terrainIndexCount = visibleTerrainIndexCount;

            if (renderer.indirect_draw_ready())
            {
                objectBatchCount = static_cast<std::uint32_t>(staticObjectScene.batches.size());
            }
            else
            {
                static std::vector<phoenix::renderer::ObjectBatch> visibleBatches;
                visibleBatches.clear();
                build_visible_object_batches(staticObjectScene, currentView, visibleBatches);
                renderer.set_static_object_batches(visibleBatches);
                std::uint32_t visibleObjectInstances{};
                for (const auto& batch : visibleBatches)
                    visibleObjectInstances += batch.instanceCount;
                objectInstanceCount = visibleObjectInstances;
                objectBatchCount = static_cast<std::uint32_t>(visibleBatches.size());
            }

            if (!animatedObjectScene.batches.empty())
            {
                static std::vector<phoenix::renderer::ObjectBatch> visibleAnimatedBatches;
                visibleAnimatedBatches.clear();
                build_visible_animated_batches(animatedObjectScene, currentView, visibleAnimatedBatches);
                renderer.set_animated_object_batches(visibleAnimatedBatches);
            }

            lastCullView = currentView;
            forceVisibilityUpdate = false;
        }

        if (imguiAvailable)
        {
            const bool botEffectsWereEnabled = botManager.effectsEnabled;
            const auto panelResult = draw_editor_panel(
                runtime,
                renderer,
                fogEnabled,
                showCollisionDebug,
                playMapSounds,
                playMapMusic,
                masterVolume,
                selectedMapIndex,
                viewDistance,
                weatherMode,
                waterMode,
                characterOptions,
                selectedCharacterOption,
                characterAppearance,
                characterSystem,
                weaponEffect,
                effectManager,
                playableMode && characterLoaded && characterSystem.ready(),
                botManager.bots.size(),
                botManager.effectsEnabled,
                botManager.weaponAurasEnabled,
                assetsFullyLoaded.load(),
                cameraX,
                cameraY,
                cameraZ,
                cameraYaw);

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
            else if (panelResult.debugGizmosChanged)
            {
                uploadDebugGizmos();
            }

            if (panelResult.characterChanged)
                reloadCharacterIntoRenderer();
            if (panelResult.emoteTriggered > 0)
                pendingEmote = panelResult.emoteTriggered;
            if (panelResult.botSpawnCount > 0 && playableMode && characterLoaded && characterSystem.ready())
            {
                botManager.spawn(panelResult.botSpawnCount, characterSystem.world_x(), characterSystem.world_z(),
                    character_height_sampler, &heightSamplerCtx);
            }
            if (panelResult.clearBots)
            {
                botManager.clear_bots();
                effectManager.clear();
                reloadCharacterIntoRenderer();
                phoenix::app::release_memory_to_os();
            }
            if (botEffectsWereEnabled && !botManager.effectsEnabled)
                effectManager.clear();

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

            static std::vector<phoenix::audio::AudibleTrack> audibleTracks;
            audibleTracks.clear();
            build_audible_tracks_into(audibleTracks,
                mapAudioScene,
                listenerX,
                listenerY,
                listenerZ,
                playMapMusic,
                playMapSounds);
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

                if (moving)
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

        // Gather all scene particles (weapon aura + world/attack/portal effects)
        // into one batch and upload once: alpha-blended first, then additive.
        particleBatch.clear();
        weaponEffect.update(deltaSeconds, characterSystem.weapon_attachment(), particleBatch);
        botManager.emit_weapon_auras(deltaSeconds, particleBatch);
        effectManager.update(deltaSeconds, particleBatch);
        if (!particleBatch.alpha.empty() || !particleBatch.additive.empty())
        {
            particleScratch.clear();
            particleScratch.reserve(particleBatch.alpha.size() + particleBatch.additive.size());
            particleScratch.insert(particleScratch.end(), particleBatch.alpha.begin(), particleBatch.alpha.end());
            const auto additiveStart = static_cast<std::uint32_t>(particleBatch.alpha.size());
            particleScratch.insert(particleScratch.end(), particleBatch.additive.begin(), particleBatch.additive.end());
            renderer.set_particle_instances(particleScratch, additiveStart);
        }
        else
        {
            particleScratch.clear();
            renderer.set_particle_instances(particleScratch, 0);
        }

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
            botManager.clear();

            // Release previous map data BEFORE loading the new one to avoid
            // both maps coexisting in RAM (doubles peak memory).
            {
                staticObjectScene.instances.clear(); staticObjectScene.instances.shrink_to_fit();
                staticObjectScene.batches.clear(); staticObjectScene.batches.shrink_to_fit();
                staticObjectScene.batchBounds.clear(); staticObjectScene.batchBounds.shrink_to_fit();
                animatedObjectScene.vertices.clear(); animatedObjectScene.vertices.shrink_to_fit();
                animatedObjectScene.indices.clear(); animatedObjectScene.indices.shrink_to_fit();
                animatedObjectScene.baseInstances.clear(); animatedObjectScene.baseInstances.shrink_to_fit();
                animatedObjectScene.instances.clear(); animatedObjectScene.instances.shrink_to_fit();
                animatedObjectScene.batches.clear(); animatedObjectScene.batches.shrink_to_fit();
                animatedObjectScene.vertexAnimations.clear(); animatedObjectScene.vertexAnimations.shrink_to_fit();
                worldCollisionMesh.triangles.clear(); worldCollisionMesh.triangles.shrink_to_fit();
                worldCollisionMesh.grid.clear();
            }

            renderer.enter_loading_mode();
            showLoading(0.05f, "Changing map");
            if (runAsync([&]() { return runtime.load_world_map(mapIdx); }, 0.10f, "Loading world"))
            {
                uploadCurrentWorld();
                applyFogSettings();

                // Apply a queued portal teleport: place the character at the portal
                // destination (raw map space -> world via halfMap), snapped to the
                // nearest walkable surface. This overrides the centre spawn that
                // uploadCurrentWorld() applies by default.
                if (pendingTeleportDestination && characterSystem.ready())
                {
                    const auto tp = *pendingTeleportDestination;
                    if (tp.hasDestination)
                    {
                        const float mapSize = static_cast<float>(runtime.state().world.mapSize);
                        const float halfMap = runtime.state().world.isDungeon ? 0.0f : mapSize * 0.5f;
                        const float wx = tp.x - halfMap;
                        const float wz = tp.z - halfMap;
                        const auto spawn = find_dungeon_playable_spawn(worldCollisionMesh, wx, tp.y, wz);
                        const float fy = spawn.valid ? spawn.y : tp.y;
                        characterSystem.set_world_position(wx, fy, wz, 0.0f);
                        heightSamplerCtx.lastCharacterY = fy;
                        uploadCharacterMesh();
                    }
                    portalCooldown = 2.0f;
                }
            }
            pendingTeleportDestination.reset();
            lastFrame = clock::now();
        }

    }

    // Join the background asset preload before tearing down (it captures
    // characterSystem/runtime by reference).
    if (backgroundAssetThread.joinable())
        backgroundAssetThread.join();

    renderer.shutdown();
    audioSystem.shutdown();
    return 0;
}
