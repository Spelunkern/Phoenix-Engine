#include "ui/weather_overlay.h"

#include "ui/phoenix_ui.h"

namespace phoenix::ui
{
    // `view`/`totalTime` are kept in the signature for callers/symmetry with
    // other overlay draws even though the (now GPU-particle-driven) rain/
    // snow/lightning no longer need them here — see WeatherParticleSystem,
    // which also owns the lightning flash formerly drawn here.
    void draw_weather_overlay(
        WeatherMode weatherMode,
        const phoenix::renderer::CameraView&,
        float /*totalTime*/,
        float width,
        float height)
    {
        if (weatherMode == WeatherMode::Default || width <= 0.0f || height <= 0.0f)
            return;

        if (weatherMode == WeatherMode::Storm)
        {
            px::background_rect({ 0.0f, 0.0f }, { width, height },
                { 24.0f / 255.0f, 28.0f / 255.0f, 34.0f / 255.0f, 20.0f / 255.0f });
            return;
        }

        if (weatherMode != WeatherMode::Snowstorm)
            return;

        px::background_rect({ 0.0f, 0.0f }, { width, height },
            { 210.0f / 255.0f, 215.0f / 255.0f, 220.0f / 255.0f, 16.0f / 255.0f });
    }
}
