#pragma once

#include "renderer/vulkan_renderer.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace phoenix::ui
{
    struct PerfHudState
    {
        static constexpr std::uint32_t kMaxCores = 64;

        // One-time system info (set at init, never changes).
        std::string osName;
        std::string cpuName;
        std::string gpuName;
        std::uint32_t cpuCores{};
        phoenix::renderer::VulkanRenderer* renderer{};

        // Icons (uploaded once, cached forever).
        bool iconsUploaded{};
        std::uint64_t windowsIcon{};
        std::uint64_t linuxIcon{};
        std::uint64_t macIcon{};
        std::uint64_t nvidiaIcon{};
        std::uint64_t amdIcon{};
        std::uint64_t intelIcon{};
        std::uint64_t cachedOsIcon{};
        std::uint64_t cachedCpuIcon{};
        std::uint64_t cachedGpuIcon{};

        // Live metrics (refreshed every ~1s via push_frametime).
        float fpsSmoothed{};
        float cpuPercent{};
        float ramUsedMB{};
        float ramTotalMB{};
        float ramPercent{};
        float vramUsedMB{};
        float vramTotalMB{};
        float processRamMB{};
        float worldX{}, worldY{}, worldZ{};
        std::string mapId;

        // FPS cap: 0=off, 1=30, 2=60, 3=120, 4=144. Persisted.
        int fpsCapIndex{};
        float fps_cap_seconds() const;

        void initialize_system_info();
        void push_frametime(float dt);
        void load_settings(const std::filesystem::path& executableDir);
        void save_settings(const std::filesystem::path& executableDir) const;

    private:
        float accumTime_{};
        int accumFrames_{};
#ifdef _WIN32
        struct CoreTimes { unsigned long long idle{}; unsigned long long kernel{}; unsigned long long user{}; };
#else
        struct CoreTimes { unsigned long long idle{}; unsigned long long total{}; };
#endif
        CoreTimes lastCoreTimes_[kMaxCores]{};
        bool cpuInitialized_{};
    };

    void draw_perf_hud(PerfHudState& hud, float surfaceWidth);
}
