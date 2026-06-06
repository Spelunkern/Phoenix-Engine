#include "ui/weather_overlay.h"

#include "world/water_constants.h"

#include "imgui.h"

#include <array>
#include <cmath>
#include <cstdint>

namespace phoenix::ui
{
    namespace
    {
        constexpr float kWeatherWaterY = phoenix::world::kWaterSurfaceY;

        float hash01(std::uint32_t value)
        {
            value ^= value >> 16;
            value *= 0x7feb352du;
            value ^= value >> 15;
            value *= 0x846ca68bu;
            value ^= value >> 16;
            return static_cast<float>(value & 0x00ffffffu) / static_cast<float>(0x01000000u);
        }

        float wrap01(float value)
        {
            return value - std::floor(value);
        }
    }

    void draw_weather_overlay(
        WeatherMode weatherMode,
        const phoenix::renderer::CameraView& view,
        float totalTime,
        float width,
        float height)
    {
        if (weatherMode == WeatherMode::Default || width <= 0.0f || height <= 0.0f)
            return;

        auto* background = ImGui::GetBackgroundDrawList();
        auto* drawList = ImGui::GetForegroundDrawList();
        if (weatherMode == WeatherMode::Storm)
        {
            background->AddRectFilled(
                ImVec2(0.0f, 0.0f),
                ImVec2(width, height),
                IM_COL32(24, 28, 34, 20));

            const float lightningCycle = std::fmod(totalTime + 2.35f, 8.7f);
            float lightning = 0.0f;
            if (lightningCycle < 0.10f)
                lightning = 1.0f - lightningCycle / 0.10f;
            else if (lightningCycle > 0.19f && lightningCycle < 0.28f)
                lightning = 0.62f * (1.0f - (lightningCycle - 0.19f) / 0.09f);
            if (lightning > 0.0f)
            {
                const int alpha = static_cast<int>(lightning * 54.0f);
                background->AddRectFilled(
                    ImVec2(0.0f, 0.0f),
                    ImVec2(width, height),
                    IM_COL32(205, 220, 238, alpha));

                const float boltX = width * (0.18f + hash01(77u + static_cast<std::uint32_t>(totalTime / 8.7f)) * 0.64f);
                std::array<ImVec2, 6> bolt{
                    ImVec2(boltX, 0.0f),
                    ImVec2(boltX - 18.0f, height * 0.12f),
                    ImVec2(boltX + 10.0f, height * 0.20f),
                    ImVec2(boltX - 26.0f, height * 0.31f),
                    ImVec2(boltX + 2.0f, height * 0.41f),
                    ImVec2(boltX - 14.0f, height * 0.52f),
                };
                background->AddPolyline(
                    bolt.data(),
                    static_cast<int>(bolt.size()),
                    IM_COL32(225, 235, 255, static_cast<int>(lightning * 148.0f)),
                    0,
                    2.0f);
            }

            constexpr std::uint32_t kDropCount = 560;
            for (std::uint32_t i = 0; i < kDropCount; ++i)
            {
                const float seedX = hash01(i * 1973u + 17u);
                const float seedY = hash01(i * 9277u + 71u);
                const float seedSpeed = hash01(i * 3181u + 131u);
                const float fall = wrap01(seedY + totalTime * (1.45f + seedSpeed * 0.55f));
                const float drift = totalTime * (85.0f + seedSpeed * 40.0f);
                const float x = std::fmod(seedX * (width + 180.0f) + drift, width + 180.0f) - 90.0f;
                const float y = fall * (height + 120.0f) - 80.0f;
                const float length = 24.0f + seedSpeed * 24.0f;
                const float slant = -10.0f - seedSpeed * 10.0f;
                const auto color = IM_COL32(185, 205, 225, static_cast<int>(92 + seedSpeed * 70.0f));
                drawList->AddLine(ImVec2(x, y), ImVec2(x + slant, y + length), color, 1.0f);
            }

            constexpr std::uint32_t kSplashCount = 96;
            for (std::uint32_t i = 0; i < kSplashCount; ++i)
            {
                const float rx = hash01(i * 1451u + 19u) - 0.5f;
                const float rz = hash01(i * 2819u + 47u) - 0.5f;
                const float phase = wrap01(hash01(i * 577u + 83u) + totalTime * (1.7f + hash01(i * 1297u) * 0.8f));
                if (phase > 0.42f)
                    continue;

                phoenix::renderer::ScreenPoint screen{};
                const float splashX = view.x + rx * 130.0f;
                const float splashZ = view.z + rz * 130.0f;
                if (!phoenix::renderer::project_world_to_screen(view, splashX, kWeatherWaterY + 0.03f, splashZ, width, height, screen))
                    continue;

                const float alpha = (1.0f - phase / 0.42f) * 105.0f;
                const float radius = 2.0f + phase * 10.0f;
                drawList->AddCircle(
                    ImVec2(screen.x, screen.y),
                    radius,
                    IM_COL32(210, 230, 240, static_cast<int>(alpha)),
                    16,
                    1.0f);
            }
            return;
        }

        if (weatherMode != WeatherMode::Snowstorm)
            return;

        background->AddRectFilled(
            ImVec2(0.0f, 0.0f),
            ImVec2(width, height),
            IM_COL32(210, 215, 220, 16));

        constexpr std::uint32_t kFlakeCount = 360;
        for (std::uint32_t i = 0; i < kFlakeCount; ++i)
        {
            const float seedX = hash01(i * 1789u + 11u);
            const float seedY = hash01(i * 3259u + 29u);
            const float seedSize = hash01(i * 811u + 101u);
            const float speed = 0.22f + seedSize * 0.34f;
            const float fall = wrap01(seedY + totalTime * speed);
            const float sway = std::sin(totalTime * (1.2f + seedSize) + seedX * 16.0f) * (12.0f + seedSize * 28.0f);
            const float x = wrap01(seedX + totalTime * 0.018f) * (width + 80.0f) - 40.0f + sway;
            const float y = fall * (height + 70.0f) - 35.0f;
            const float radius = 1.1f + seedSize * 2.1f;
            const auto alpha = static_cast<int>(120.0f + seedSize * 95.0f);
            drawList->AddCircleFilled(ImVec2(x, y), radius, IM_COL32(245, 248, 255, alpha), 8);
        }
    }
}
