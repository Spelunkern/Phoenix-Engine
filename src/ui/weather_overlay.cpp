#include "ui/weather_overlay.h"

#include "imgui.h"

namespace phoenix::ui
{
    // `view`/`totalTime` are kept in the signature for callers/symmetry with
    // other overlay draws even though the (now GPU-particle-driven) rain/
    // snow/lightning no longer need them here — see WeatherParticleSystem,
    // which also owns the lightning flash that used to be drawn here via
    // ImGui.
    void draw_weather_overlay(
        WeatherMode weatherMode,
        const phoenix::renderer::CameraView&,
        float /*totalTime*/,
        float width,
        float height)
    {
        if (weatherMode == WeatherMode::Default || width <= 0.0f || height <= 0.0f)
            return;

        auto* background = ImGui::GetBackgroundDrawList();
        if (weatherMode == WeatherMode::Storm)
        {
            background->AddRectFilled(
                ImVec2(0.0f, 0.0f),
                ImVec2(width, height),
                IM_COL32(24, 28, 34, 20));
            return;
        }

        if (weatherMode != WeatherMode::Snowstorm)
            return;

        background->AddRectFilled(
            ImVec2(0.0f, 0.0f),
            ImVec2(width, height),
            IM_COL32(210, 215, 220, 16));
    }
}
