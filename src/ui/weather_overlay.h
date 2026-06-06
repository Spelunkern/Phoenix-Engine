#pragma once

#include "renderer/visibility_culling.h"
#include "ui/editor_panel.h"

namespace phoenix::ui
{
    void draw_weather_overlay(
        WeatherMode weatherMode,
        const phoenix::renderer::CameraView& view,
        float totalTime,
        float width,
        float height);
}
