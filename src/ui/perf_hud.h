#pragma once

#include "renderer/opengl_renderer.h"

#include <array>
#include <cstdint>
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
        phoenix::renderer::OpenGLRenderer* renderer{};

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

        // Graphics settings are edited in the unified Graphics panel and
        // persisted via app_settings.h.
        int fpsCapIndex{};
        float fps_cap_seconds() const;

        // MSAA + alpha-to-coverage toggle (see OpenGLRenderer::set_antialiasing_enabled).
        // Persisted via app_settings.h alongside the other fields above.
        // Hidden/disabled in the HUD if the GL context didn't get multisample
        // buffers (set by main.cpp from SdlWindow::has_multisample_context()).
        bool antialiasingEnabled{ true };
        bool antialiasingAvailable{ true };

        void initialize_system_info();
        void push_frametime(float dt);

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
