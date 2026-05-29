#pragma once

#include "renderer/vulkan_renderer.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace phoenix::ui
{
    // Floating performance HUD: FPS/frametime graph, per-core CPU, RAM and VRAM.
    // System metrics are gathered per-platform (Win32 APIs / Linux /proc) and
    // refreshed twice a second from push_frametime().
    struct PerfHudState
    {
        static constexpr std::size_t kHistorySize = 128;
        static constexpr std::uint32_t kMaxCores = 64;
        float frametimeHistory[kHistorySize]{};
        std::size_t historyIndex{};
        float fpsSmoothed{};
        float frametimeMs{};
        float frametimeMin{ 999.0f };
        float frametimeMax{};
        float frametimeAvg{};
        std::uint32_t visibleBatches{};
        std::uint32_t visibleInstances{};
        std::uint32_t totalTriangles{};
        std::uint32_t actorCount{};
        std::uint32_t mobsMoving{};
        bool visible{ true };

        // System metrics (updated periodically).
        float ramUsedMB{};
        float ramTotalMB{};
        float ramPercent{};
        float processRamMB{};
        float cpuPercent{};
        std::uint32_t cpuCores{};
        float coreUsage[kMaxCores]{};
        std::string gpuName;
        float vramUsedMB{};
        float vramTotalMB{};
        float systemUpdateTimer{};
        phoenix::renderer::VulkanRenderer* renderer{};

        // Per-core CPU state (raw counters from the previous sample).
#ifdef _WIN32
        struct CoreTimes { unsigned long long idle{}; unsigned long long kernel{}; unsigned long long user{}; };
#else
        struct CoreTimes { unsigned long long idle{}; unsigned long long total{}; };
#endif
        CoreTimes lastCoreTimes[kMaxCores]{};
        bool cpuInitialized{};

        void initialize_system_info();
        void update_system_metrics();
        void push_frametime(float dt);
    };

    void draw_perf_hud(PerfHudState& hud, float surfaceWidth);
}
