#include "ui/editor_panel.h"
#include "ui/app_settings.h"
#include "ui/cpu_profiler.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace phoenix::ui
{
    namespace
    {
        constexpr float kFogStartRatio = 0.38f;   // fog begins at 38% of viewDistance
        constexpr float kFogEndRatio = 0.82f;     // fog is ~100% opaque at 82% of viewDistance
    }

// === moved bodies appended below by build step ===
    float apply_renderer_fog(
        phoenix::renderer::OpenGLRenderer& renderer,
        const phoenix::runtime::PhoenixRuntime& runtime,
        bool fogEnabled,
        float viewDistance,
        WeatherMode weatherMode)
    {
        const auto& world = runtime.state().world;
        // Dungeons always use pitch-black sky and fog — no clouds, no sky texture.
        const bool dungeon = world.isDungeon;
        std::array<float, 3> weatherFog{
            dungeon ? 0.0f : world.fogColor[0],
            dungeon ? 0.0f : world.fogColor[1],
            dungeon ? 0.0f : world.fogColor[2],
        };
        phoenix::renderer::EnvironmentStyle environment{
            { 0.76f, 0.85f, 0.94f, 0.24f },
            { 0.13f, 0.34f, 0.72f, 0.30f },
            { 0.43f, 0.65f, 0.88f, 0.30f },
            { 0.30f, 0.28f, 0.26f, 0.30f },
            { 1.00f, 0.99f, 0.96f, 1.00f },
            { 0.58f, 0.65f, 0.76f, 0.95f },
            { 0.34f, 0.46f, 0.25f, 40.0f },
            { 0.60f, 0.20f, 0.10f, 0.0f },
            { 0.18f, 0.46f, 0.54f, 0.18f },
            { 0.025f, 0.13f, 0.27f, 0.94f },
            { 0.05f, 0.030f, 0.58f, 4.0f },
            { 0.75f, 0.22f, 6.0f, 0.04f },
            { -0.4575f, 0.6691f, 0.5855f, 1.18f },
            { -0.5708f, 0.3746f, 0.7305f, 0.42f },
            { 1.00f, 0.93f, 0.78f, 0.70f },
            { 0.54f, 0.62f, 0.74f, 0.48f },
            { 1.00f, 0.94f, 0.78f, 18.0f },
            { 1.00f, 0.98f, 0.90f, 0.62f },
            { 1.05f, 0.0f, 0.014f, 0.0f },
            { 0.92f, 0.95f, 1.00f, 220.0f },
            { 0.10f, 0.85f, 0.45f, 0.30f },
            { 0.35f, 0.25f, 0.85f, 0.06f },
            { 0.0f, 0.5f, 0.0f, 0.0f },
            { 0.95f, 0.97f, 1.00f, 0.0f },
        };
        const auto set4 = [](float (&target)[4], float x, float y, float z, float w) {
            target[0] = x; target[1] = y; target[2] = z; target[3] = w;
        };
        const auto set_direction = [](float (&target)[4], float elevationDegrees,
            float azimuthDegrees, float value) {
            constexpr float degreesToRadians = 3.14159265358979323846f / 180.0f;
            const auto elevation = elevationDegrees * degreesToRadians;
            const auto azimuth = azimuthDegrees * degreesToRadians;
            const auto horizontal = std::cos(elevation);
            target[0] = horizontal * std::sin(azimuth);
            target[1] = std::sin(elevation);
            target[2] = horizontal * std::cos(azimuth);
            target[3] = value;
        };
        if (weatherMode == WeatherMode::ClearDay)
        {
            weatherFog = { 0.70f, 0.84f, 0.96f };
            set4(environment.horizonCurve, 0.70f, 0.84f, 0.96f, 0.25f);
            set4(environment.zenithMidWeight, 0.08f, 0.29f, 0.70f, 0.25f);
            set4(environment.midHeight, 0.42f, 0.67f, 0.91f, 0.29f);
            set4(environment.groundCloudCover, 0.30f, 0.28f, 0.26f, 0.28f);
            set4(environment.cloudColorOpacity, 1.00f, 1.00f, 0.98f, 0.98f);
            set4(environment.cloudShadeSpeed, 0.64f, 0.72f, 0.84f, 1.10f);
            set4(environment.cloudShape, 0.31f, 0.48f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.19f, 0.49f, 0.57f, 0.18f);
            set4(environment.waterDeepAlpha, 0.02f, 0.14f, 0.29f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.030f, 0.60f, 4.0f);
        }
        else if (weatherMode == WeatherMode::MistyMorning)
        {
            weatherFog = { 0.76f, 0.76f, 0.72f };
            set4(environment.horizonCurve, 0.90f, 0.86f, 0.78f, 0.50f);
            set4(environment.zenithMidWeight, 0.56f, 0.64f, 0.70f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.34f, 0.33f, 0.30f, 0.55f);
            set4(environment.cloudColorOpacity, 0.94f, 0.93f, 0.90f, 0.90f);
            set4(environment.cloudShadeSpeed, 0.70f, 0.70f, 0.72f, 0.60f);
            set4(environment.cloudShape, 0.29f, 0.52f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.30f, 0.38f, 0.40f, 0.18f);
            set4(environment.waterDeepAlpha, 0.09f, 0.15f, 0.20f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.018f, 0.35f, 4.0f);
        }
        else if (weatherMode == WeatherMode::BlueHour)
        {
            weatherFog = { 0.10f, 0.13f, 0.18f };
            set4(environment.horizonCurve, 0.32f, 0.48f, 0.70f, 0.30f);
            set4(environment.zenithMidWeight, 0.06f, 0.12f, 0.34f, 0.40f);
            set4(environment.midHeight, 0.20f, 0.34f, 0.60f, 0.30f);
            set4(environment.groundCloudCover, 0.10f, 0.13f, 0.18f, 0.32f);
            set4(environment.cloudColorOpacity, 0.62f, 0.68f, 0.84f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.28f, 0.32f, 0.46f, 0.60f);
            set4(environment.cloudShape, 0.33f, 0.42f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.10f, 0.20f, 0.36f, 0.18f);
            set4(environment.waterDeepAlpha, 0.01f, 0.04f, 0.13f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.022f, 0.45f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Aurora)
        {
            weatherFog = { 0.080f, 0.105f, 0.160f };
            set4(environment.horizonCurve, 0.075f, 0.100f, 0.180f, 0.28f);
            set4(environment.zenithMidWeight, 0.018f, 0.035f, 0.085f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.045f, 0.050f, 0.070f, 0.0f);
            set4(environment.cloudColorOpacity, 0.0f, 0.0f, 0.0f, 0.0f);
            set4(environment.waterShallowAlpha, 0.24f, 0.44f, 0.52f, 0.18f);
            set4(environment.waterDeepAlpha, 0.10f, 0.22f, 0.32f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.020f, 0.40f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Storm)
        {
            weatherFog = { 0.20f, 0.22f, 0.25f };
            set4(environment.horizonCurve, 0.34f, 0.36f, 0.40f, 0.40f);
            set4(environment.zenithMidWeight, 0.13f, 0.15f, 0.19f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.14f, 0.15f, 0.16f, 0.90f);
            set4(environment.cloudColorOpacity, 0.46f, 0.48f, 0.54f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.20f, 0.21f, 0.25f, 2.2f);
            set4(environment.cloudShape, 0.23f, 0.54f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.16f, 0.24f, 0.24f, 0.18f);
            set4(environment.waterDeepAlpha, 0.03f, 0.07f, 0.08f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.075f, 1.60f, 4.0f);
            set4(environment.groundWeather, 1.0f, 0.5f, 0.0f, 0.0f);
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            weatherFog = { 0.55f, 0.57f, 0.60f };
            set4(environment.horizonCurve, 0.86f, 0.89f, 0.93f, 0.42f);
            set4(environment.zenithMidWeight, 0.52f, 0.62f, 0.74f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.34f, 0.36f, 0.38f, 0.70f);
            set4(environment.cloudColorOpacity, 0.94f, 0.96f, 1.00f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.68f, 0.72f, 0.80f, 1.1f);
            set4(environment.cloudShape, 0.27f, 0.50f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.30f, 0.44f, 0.50f, 0.18f);
            set4(environment.waterDeepAlpha, 0.07f, 0.16f, 0.24f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.026f, 0.50f, 4.0f);
            set4(environment.groundWeather, 0.0f, 0.5f, 0.62f, 0.0f);
            set4(environment.snowColor, 0.95f, 0.97f, 1.00f, 0.0f);
        }
        else if (weatherMode == WeatherMode::Sunset)
        {
            weatherFog = { 0.78f, 0.42f, 0.24f };
            set4(environment.horizonCurve, 1.00f, 0.52f, 0.28f, 0.34f);
            set4(environment.zenithMidWeight, 0.16f, 0.20f, 0.48f, 0.65f);
            set4(environment.midHeight, 0.82f, 0.40f, 0.50f, 0.24f);
            set4(environment.groundCloudCover, 0.24f, 0.18f, 0.18f, 0.45f);
            set4(environment.cloudColorOpacity, 1.00f, 0.84f, 0.70f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.50f, 0.40f, 0.46f, 0.7f);
            set4(environment.cloudShape, 0.34f, 0.40f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.34f, 0.32f, 0.38f, 0.18f);
            set4(environment.waterDeepAlpha, 0.07f, 0.07f, 0.18f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.024f, 0.50f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Night)
        {
            weatherFog = { 0.035f, 0.045f, 0.075f };
            set4(environment.horizonCurve, 0.075f, 0.100f, 0.180f, 0.28f);
            set4(environment.zenithMidWeight, 0.018f, 0.035f, 0.085f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.045f, 0.050f, 0.070f, 0.0f);
            set4(environment.cloudColorOpacity, 0.42f, 0.46f, 0.58f, 0.0f);
            set4(environment.cloudShadeSpeed, 0.16f, 0.18f, 0.26f, 0.5f);
            set4(environment.cloudShape, 0.33f, 0.42f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.22f, 0.32f, 0.46f, 0.18f);
            set4(environment.waterDeepAlpha, 0.10f, 0.17f, 0.28f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.018f, 0.40f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Dawn)
        {
            weatherFog = { 0.62f, 0.48f, 0.42f };
            set4(environment.horizonCurve, 0.98f, 0.72f, 0.48f, 0.30f);
            set4(environment.zenithMidWeight, 0.10f, 0.20f, 0.48f, 0.55f);
            set4(environment.midHeight, 0.55f, 0.42f, 0.62f, 0.26f);
            set4(environment.groundCloudCover, 0.24f, 0.20f, 0.20f, 0.40f);
            set4(environment.cloudColorOpacity, 1.00f, 0.90f, 0.82f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.58f, 0.50f, 0.56f, 0.8f);
            set4(environment.cloudShape, 0.33f, 0.42f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.28f, 0.36f, 0.44f, 0.18f);
            set4(environment.waterDeepAlpha, 0.05f, 0.09f, 0.20f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.022f, 0.45f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Dusk)
        {
            weatherFog = { 0.38f, 0.22f, 0.32f };
            set4(environment.horizonCurve, 0.74f, 0.44f, 0.38f, 0.36f);
            set4(environment.zenithMidWeight, 0.10f, 0.13f, 0.32f, 0.55f);
            set4(environment.midHeight, 0.46f, 0.32f, 0.54f, 0.22f);
            set4(environment.groundCloudCover, 0.18f, 0.16f, 0.18f, 0.38f);
            set4(environment.cloudColorOpacity, 0.72f, 0.72f, 0.82f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.34f, 0.34f, 0.44f, 0.6f);
            set4(environment.cloudShape, 0.34f, 0.40f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.16f, 0.22f, 0.34f, 0.18f);
            set4(environment.waterDeepAlpha, 0.02f, 0.05f, 0.14f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.024f, 0.50f, 4.0f);
        }
        else if (weatherMode == WeatherMode::MidAfternoon)
        {
            weatherFog = { 0.82f, 0.72f, 0.52f };
            set4(environment.horizonCurve, 0.88f, 0.82f, 0.68f, 0.20f);
            set4(environment.zenithMidWeight, 0.20f, 0.42f, 0.76f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.30f, 0.27f, 0.23f, 0.30f);
            set4(environment.cloudColorOpacity, 1.00f, 0.98f, 0.94f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.66f, 0.68f, 0.72f, 1.0f);
            set4(environment.cloudShape, 0.31f, 0.44f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.18f, 0.44f, 0.50f, 0.18f);
            set4(environment.waterDeepAlpha, 0.03f, 0.12f, 0.24f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.030f, 0.55f, 4.0f);
        }
        else if (weatherMode == WeatherMode::Overcast)
        {
            weatherFog = { 0.52f, 0.54f, 0.56f };
            set4(environment.horizonCurve, 0.66f, 0.68f, 0.70f, 0.45f);
            set4(environment.zenithMidWeight, 0.44f, 0.47f, 0.52f, 0.0f);
            set4(environment.midHeight, 0.0f, 0.0f, 0.0f, 0.35f);
            set4(environment.groundCloudCover, 0.26f, 0.26f, 0.26f, 0.85f);
            set4(environment.cloudColorOpacity, 0.80f, 0.82f, 0.85f, 1.0f);
            set4(environment.cloudShadeSpeed, 0.52f, 0.55f, 0.60f, 1.4f);
            set4(environment.cloudShape, 0.25f, 0.52f, 0.25f, 40.0f);
            set4(environment.waterShallowAlpha, 0.20f, 0.30f, 0.34f, 0.18f);
            set4(environment.waterDeepAlpha, 0.04f, 0.09f, 0.14f, 0.94f);
            set4(environment.waterSurface, 0.05f, 0.042f, 0.85f, 4.0f);
            set4(environment.groundWeather, 0.18f, 0.5f, 0.0f, 0.0f);
        }

        // Port the complete lighting/astro side of the corresponding Godot
        // preset. The visible astro may deliberately sit lower than the
        // directional light, exactly as it does in SkyPresets.
        switch (weatherMode)
        {
            case WeatherMode::ClearDay:
                set_direction(environment.lightDirectionEnergy, 47.0f, 35.0f, 1.28f);
                set_direction(environment.astroDirectionGlow, 24.0f, 35.0f, 0.46f);
                set4(environment.lightColorShadow, 1.00f, 0.96f, 0.84f, 0.70f);
                set4(environment.ambientColorEnergy, 0.55f, 0.64f, 0.77f, 0.48f);
                set4(environment.glowColorFocus, 1.00f, 0.96f, 0.84f, 20.0f);
                set4(environment.diskColorSize, 1.00f, 0.99f, 0.92f, 0.56f);
                set4(environment.skyOptics, 1.08f, 0.0f, 0.014f, 0.0f);
                break;
            case WeatherMode::Dawn:
                set_direction(environment.lightDirectionEnergy, 9.0f, 95.0f, 0.95f);
                set_direction(environment.astroDirectionGlow, 8.0f, 95.0f, 0.85f);
                set4(environment.lightColorShadow, 1.00f, 0.80f, 0.60f, 0.72f);
                set4(environment.ambientColorEnergy, 0.48f, 0.46f, 0.58f, 0.40f);
                set4(environment.glowColorFocus, 1.00f, 0.74f, 0.42f, 10.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::MidAfternoon:
                set_direction(environment.lightDirectionEnergy, 34.0f, -50.0f, 1.20f);
                set_direction(environment.astroDirectionGlow, 20.0f, -50.0f, 0.45f);
                set4(environment.lightColorShadow, 1.00f, 0.92f, 0.76f, 0.70f);
                set4(environment.ambientColorEnergy, 0.58f, 0.56f, 0.52f, 0.42f);
                set4(environment.glowColorFocus, 1.00f, 0.90f, 0.68f, 14.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Dusk:
                set_direction(environment.lightDirectionEnergy, 3.0f, -120.0f, 0.75f);
                set_direction(environment.astroDirectionGlow, 3.0f, -120.0f, 0.60f);
                set4(environment.lightColorShadow, 0.88f, 0.70f, 0.66f, 0.68f);
                set4(environment.ambientColorEnergy, 0.42f, 0.40f, 0.50f, 0.46f);
                set4(environment.glowColorFocus, 0.92f, 0.50f, 0.32f, 12.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Sunset:
                set_direction(environment.lightDirectionEnergy, 7.0f, -110.0f, 1.15f);
                set_direction(environment.astroDirectionGlow, 6.0f, -110.0f, 1.10f);
                set4(environment.lightColorShadow, 1.00f, 0.74f, 0.52f, 0.72f);
                set4(environment.ambientColorEnergy, 0.58f, 0.48f, 0.52f, 0.50f);
                set4(environment.glowColorFocus, 1.00f, 0.58f, 0.28f, 8.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Night:
                set_direction(environment.lightDirectionEnergy, 34.0f, 150.0f, 0.46f);
                set_direction(environment.astroDirectionGlow, 14.0f, 150.0f, 0.30f);
                set4(environment.lightColorShadow, 0.62f, 0.72f, 0.94f, 0.62f);
                set4(environment.ambientColorEnergy, 0.25f, 0.30f, 0.44f, 0.52f);
                set4(environment.glowColorFocus, 0.62f, 0.70f, 0.88f, 90.0f);
                set4(environment.diskColorSize, 0.93f, 0.95f, 1.00f, 1.50f);
                set4(environment.skyOptics, 1.15f, 1.0f, 0.045f, 0.0f);
                break;
            case WeatherMode::Overcast:
                set_direction(environment.lightDirectionEnergy, 55.0f, 20.0f, 0.55f);
                set_direction(environment.astroDirectionGlow, 55.0f, 20.0f, 0.0f);
                set4(environment.lightColorShadow, 0.86f, 0.88f, 0.92f, 0.42f);
                set4(environment.ambientColorEnergy, 0.62f, 0.64f, 0.68f, 0.70f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Storm:
                set_direction(environment.lightDirectionEnergy, 40.0f, 70.0f, 0.35f);
                set_direction(environment.astroDirectionGlow, 40.0f, 70.0f, 0.0f);
                set4(environment.lightColorShadow, 0.66f, 0.70f, 0.78f, 0.55f);
                set4(environment.ambientColorEnergy, 0.30f, 0.33f, 0.38f, 0.55f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Snowstorm:
                set_direction(environment.lightDirectionEnergy, 30.0f, -25.0f, 0.70f);
                set_direction(environment.astroDirectionGlow, 30.0f, -25.0f, 0.0f);
                set4(environment.lightColorShadow, 0.90f, 0.94f, 1.00f, 0.42f);
                set4(environment.ambientColorEnergy, 0.72f, 0.78f, 0.86f, 0.72f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::MistyMorning:
                set_direction(environment.lightDirectionEnergy, 14.0f, 80.0f, 0.55f);
                set_direction(environment.astroDirectionGlow, 10.0f, 80.0f, 0.40f);
                set4(environment.lightColorShadow, 1.00f, 0.95f, 0.86f, 0.40f);
                set4(environment.ambientColorEnergy, 0.76f, 0.76f, 0.72f, 0.78f);
                set4(environment.glowColorFocus, 1.00f, 0.94f, 0.80f, 5.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::BlueHour:
                set_direction(environment.lightDirectionEnergy, -4.0f, -130.0f, 0.42f);
                set_direction(environment.astroDirectionGlow, -4.0f, -130.0f, 0.35f);
                set4(environment.lightColorShadow, 0.62f, 0.72f, 0.94f, 0.60f);
                set4(environment.ambientColorEnergy, 0.34f, 0.42f, 0.58f, 0.50f);
                set4(environment.glowColorFocus, 0.70f, 0.60f, 0.62f, 14.0f);
                set4(environment.diskColorSize, 1.0f, 1.0f, 1.0f, 0.0f);
                break;
            case WeatherMode::Aurora:
                set_direction(environment.lightDirectionEnergy, 55.0f, 165.0f, 0.46f);
                set_direction(environment.astroDirectionGlow, 20.0f, 165.0f, 0.25f);
                set4(environment.lightColorShadow, 0.62f, 0.72f, 0.94f, 0.62f);
                set4(environment.ambientColorEnergy, 0.25f, 0.30f, 0.44f, 0.52f);
                set4(environment.glowColorFocus, 0.55f, 0.62f, 0.80f, 28.0f);
                set4(environment.diskColorSize, 0.93f, 0.95f, 1.00f, 1.50f);
                set4(environment.skyOptics, 1.05f, 1.0f, 0.045f, 1.0f);
                set4(environment.auroraLowSpread, 0.10f, 0.85f, 0.45f, 0.30f);
                set4(environment.auroraHighSpeed, 0.35f, 0.25f, 0.85f, 0.06f);
                break;
            case WeatherMode::Default:
            default:
                break;
        }
        if (dungeon)
        {
            set4(environment.lightDirectionEnergy, 0.0f, 1.0f, 0.0f, 0.0f);
            set4(environment.ambientColorEnergy, 1.0f, 1.0f, 1.0f, 1.0f);
            set4(environment.astroDirectionGlow, 0.0f, 1.0f, 0.0f, 0.0f);
            set4(environment.skyOptics, 0.0f, 0.0f, 0.0f, 0.0f);
        }
        renderer.set_environment_style(environment);
        if (!fogEnabled && !dungeon)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                100000.0f,
                100001.0f,
                world.parsedSky && !world.skyFileName.empty());
            return viewDistance;
        }
        // Dungeons always have fog (black) even when the user disables the fog
        // checkbox, because the alternative is seeing the skybox through walls.
        if (!fogEnabled && dungeon)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                viewDistance * 0.7f,
                viewDistance,
                false);
            return viewDistance;
        }

        // Atmospheric fog: starts gradually, reaches full opacity well before cull edge.
        auto fogStart = std::max(80.0f, viewDistance * kFogStartRatio);
        auto fogEnd = std::max(fogStart + 100.0f, viewDistance * kFogEndRatio);
        if (weatherMode == WeatherMode::Storm)
        {
            fogStart = std::max(40.0f, viewDistance * 0.22f);
            fogEnd = std::max(fogStart + 90.0f, viewDistance * 0.64f);
        }
        else if (weatherMode == WeatherMode::Snowstorm)
        {
            fogStart = std::max(28.0f, viewDistance * 0.16f);
            fogEnd = std::max(fogStart + 75.0f, viewDistance * 0.52f);
        }
        else if (weatherMode == WeatherMode::Overcast)
        {
            fogStart = std::max(60.0f, viewDistance * 0.28f);
            fogEnd = std::max(fogStart + 100.0f, viewDistance * 0.72f);
        }
        if (dungeon)
        {
            renderer.set_sky_settings(
                weatherFog.data(),
                viewDistance * 0.7f,
                viewDistance,
                false);
            return viewDistance;
        }
        renderer.set_sky_settings(
            weatherFog.data(),
            fogStart,
            fogEnd,
            world.parsedSky && !world.skyFileName.empty());
        return fogEnd;
    }

    int nearest_available(int value, const std::vector<int>& values)
    {
        if (values.empty())
            return value;
        auto best = values.front();
        auto bestDistance = std::abs(best - value);
        for (const auto candidate : values)
        {
            const auto distance = std::abs(candidate - value);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

    UnifiedPanelResult draw_editor_panel(
        const phoenix::runtime::PhoenixRuntime& runtime,
        phoenix::renderer::OpenGLRenderer& renderer,
        bool& fogEnabled,
        bool& worldShadows,
        int& fpsCapIndex,
        bool& antialiasingEnabled,
        bool antialiasingAvailable,
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
        const std::vector<std::string>& effectFileNames,
        int& selectedEffectFileIndex,
        const std::vector<std::string>& effectSequenceNames,
        int& selectedEffectSequenceIndex,
        std::size_t effectActiveCount,
        float& effectSpawnYOffset,
        int& effectComponentIndex,
        bool assetsReady)
    {
        UnifiedPanelResult result{};
        const auto prevAppearance = appearance;
        const auto prevCharOption = selectedCharacterOption;

        enum class Section : int
        {
            Map,
            Graphics,
            Sound,
            Character,
            Vehicle,
            Bots,
            NPCs,
            Monsters,
            Animations,
            Effects,
        };
        static Section activeSection = Section::Map;

        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(300.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Phoenix Engine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::End();
            return result;
        }

        auto sectionButton = [&](Section section, const char* label) {
            const bool selected = activeSection == section;
            if (selected)
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(label, ImVec2(68.0f, 0.0f)))
                activeSection = section;
            if (selected)
                ImGui::PopStyleColor();
        };

        sectionButton(Section::Map, "Map##nav"); ImGui::SameLine();
        sectionButton(Section::Graphics, "Graphics##nav"); ImGui::SameLine();
        sectionButton(Section::Sound, "Sound##nav"); ImGui::SameLine();
        sectionButton(Section::Animations, "Animations##nav"); ImGui::SameLine();
        sectionButton(Section::Effects, "Effects##nav");
        sectionButton(Section::Character, "Character##nav"); ImGui::SameLine();
        sectionButton(Section::Vehicle, "Vehicle##nav"); ImGui::SameLine();
        sectionButton(Section::Bots, "Bots##nav"); ImGui::SameLine();
        sectionButton(Section::NPCs, "NPCs##nav"); ImGui::SameLine();
        sectionButton(Section::Monsters, "Monsters##nav");
        ImGui::Separator();

        if (activeSection == Section::Map)
        {
            const auto& maps = runtime.world_map_names();
            if (!maps.empty())
            {
                selectedMapIndex = std::clamp(selectedMapIndex, 0, static_cast<int>(maps.size() - 1));
                ImGui::SetNextItemWidth(200.0f);
                if (ImGui::BeginCombo("Map##mapCombo", maps[static_cast<std::size_t>(selectedMapIndex)].c_str()))
                {
                    for (std::size_t i = 0; i < maps.size(); ++i)
                    {
                        if (ImGui::Selectable(maps[i].c_str(), selectedMapIndex == static_cast<int>(i)))
                            selectedMapIndex = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                result.loadRequested = ImGui::Button("Load");
            }

            const auto previousFog = fogEnabled;
            ImGui::Checkbox("Fog", &fogEnabled);
            if (fogEnabled != previousFog)
                apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);

            const auto previousViewDistance = viewDistance;
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat("Fog distance", &viewDistance, 100.0f, 2500.0f, "%.0f");
            result.viewDistanceChanged = std::abs(previousViewDistance - viewDistance) > 1.0f;

            const WeatherMode previousWeatherMode = weatherMode;
            const char* weatherItems[] = { "Default", "Clear day", "Dawn", "Afternoon",
                "Sunset", "Dusk", "Night", "Overcast", "Snow", "Misty morning",
                "Blue hour", "Aurora", "Storm" };
            int weatherIndex = static_cast<int>(weatherMode);
            ImGui::SetNextItemWidth(180.0f);
            if (ImGui::Combo("Sky", &weatherIndex, weatherItems, IM_ARRAYSIZE(weatherItems)))
                weatherMode = static_cast<WeatherMode>(std::clamp(weatherIndex, 0, 12));
            result.weatherChanged = weatherMode != previousWeatherMode;
        }
        else if (activeSection == Section::Graphics)
        {
            if (ImGui::Checkbox("World shadows", &worldShadows))
            {
                renderer.set_shadows_enabled(worldShadows);
                flush_app_settings();
            }

            if (antialiasingAvailable)
            {
                if (ImGui::Checkbox("Anti-aliasing", &antialiasingEnabled))
                {
                    renderer.set_antialiasing_enabled(antialiasingEnabled);
                    flush_app_settings();
                }
            }
            else
            {
                ImGui::TextDisabled("Anti-aliasing unavailable");
            }

            const char* caps[] = { "Off", "30", "60", "75", "90", "120", "144", "165", "240", "360" };
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::Combo("FPS cap", &fpsCapIndex, caps, IM_ARRAYSIZE(caps)))
                flush_app_settings();
        }
        else if (activeSection == Section::Sound)
        {
            ImGui::Checkbox("Play Sounds", &playMapSounds);
            ImGui::Checkbox("Play Music", &playMapMusic);
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f, "%.2f");
        }
        else if (activeSection == Section::Character)
        {
            if (characterOptions.empty())
            {
                ImGui::TextDisabled("No character models found.");
            }
            else if (!assetsReady)
            {
                ImGui::TextDisabled("Loading assets...");
            }
            else
            {
                selectedCharacterOption = std::clamp(selectedCharacterOption, 0, static_cast<int>(characterOptions.size() - 1));
                const auto& selected = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
                ImGui::SetNextItemWidth(190.0f);
                if (ImGui::BeginCombo("Model", selected.label.c_str()))
                {
                    for (std::size_t i = 0; i < characterOptions.size(); ++i)
                    {
                        const bool isSelected = selectedCharacterOption == static_cast<int>(i);
                        if (ImGui::Selectable(characterOptions[i].label.c_str(), isSelected))
                        {
                            selectedCharacterOption = static_cast<int>(i);
                            appearance.raceFolder = characterOptions[i].raceFolder;
                            appearance.prefix = characterOptions[i].prefix;
                            appearance.upperIndex = nearest_available(appearance.upperIndex, characterOptions[i].upperIndices);
                            appearance.lowerIndex = nearest_available(appearance.lowerIndex, characterOptions[i].lowerIndices);
                            appearance.handIndex = nearest_available(appearance.handIndex, characterOptions[i].handIndices);
                            appearance.footIndex = nearest_available(appearance.footIndex, characterOptions[i].footIndices);
                            appearance.helmetIndex = nearest_available(appearance.helmetIndex, characterOptions[i].helmetIndices);
                            appearance.faceIndex = nearest_available(appearance.faceIndex, characterOptions[i].faceIndices);
                            appearance.hairIndex = nearest_available(appearance.hairIndex, characterOptions[i].hairIndices);
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                const auto& current = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
                appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
                appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
                appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
                appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
                appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
                appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
                appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);

                ImGui::Checkbox("Helmet", &appearance.helmetVisible);
                if (appearance.helmetVisible)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("##helmet", &appearance.helmetIndex);
                    appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
                }
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Upper", &appearance.upperIndex);
                appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Lower", &appearance.lowerIndex);
                appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Gloves", &appearance.handIndex);
                appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Boots", &appearance.footIndex);
                appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
                ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Face", &appearance.faceIndex);
                appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
                if (!appearance.helmetVisible)
                {
                    ImGui::SetNextItemWidth(80.0f); ImGui::InputInt("Hair", &appearance.hairIndex);
                    appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
                }

                ImGui::Separator();
                using WT = phoenix::character::WeaponType;
                struct WeaponLabel { WT type; const char* label; };
                static constexpr WeaponLabel weaponLabels[] = {
                    { WT::None, "None" }, { WT::Sword1H, "Sword 1H" }, { WT::Sword2H, "Sword 2H" },
                    { WT::Axe1H, "Axe 1H" }, { WT::Axe2H, "Axe 2H" }, { WT::DualSword, "Dual Sword" },
                    { WT::Spear, "Spear" }, { WT::Mace1H, "Mace 1H" }, { WT::Hammer2H, "Hammer 2H" },
                    { WT::RevDagger, "Rev Dagger" }, { WT::Dagger, "Dagger" }, { WT::Javelin, "Javelin" },
                    { WT::Staff, "Staff" }, { WT::Bow, "Bow" }, { WT::Crossbow, "Crossbow" }, { WT::Claw, "Claw" },
                };
                static constexpr WeaponLabel shieldLabels[] = {
                    { WT::None, "None" }, { WT::ShieldLight, "Shield (Light)" }, { WT::ShieldDark, "Shield (Dark)" },
                };

                const char* currentWeaponLabel = "None";
                int currentWeaponIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    if (weaponLabels[i].type == appearance.weaponType) { currentWeaponLabel = weaponLabels[i].label; currentWeaponIdx = i; break; }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Weapon", currentWeaponLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    {
                        const bool isSelected = i == currentWeaponIdx;
                        if (ImGui::Selectable(weaponLabels[i].label, isSelected))
                        {
                            appearance.weaponType = weaponLabels[i].type;
                            if (appearance.weaponType == WT::None) appearance.weaponIndex = -1;
                            else if (appearance.weaponIndex < 0) appearance.weaponIndex = 1;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (appearance.weaponType != WT::None)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##weapIdx", &appearance.weaponIndex);
                    if (appearance.weaponIndex < 1) appearance.weaponIndex = 1;
                }

                const char* currentShieldLabel = "None";
                int currentShieldIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    if (shieldLabels[i].type == appearance.shieldType) { currentShieldLabel = shieldLabels[i].label; currentShieldIdx = i; break; }
                ImGui::SetNextItemWidth(140.0f);
                if (ImGui::BeginCombo("Shield", currentShieldLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    {
                        const bool isSelected = i == currentShieldIdx;
                        if (ImGui::Selectable(shieldLabels[i].label, isSelected))
                        {
                            appearance.shieldType = shieldLabels[i].type;
                            if (appearance.shieldType == WT::None) appearance.shieldIndex = -1;
                            else if (appearance.shieldIndex < 0) appearance.shieldIndex = 1;
                        }
                        if (isSelected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (appearance.shieldType != WT::None)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##shldIdx", &appearance.shieldIndex);
                    if (appearance.shieldIndex < 1) appearance.shieldIndex = 1;
                }

                ImGui::Separator();
                bool hasCloak = appearance.cloakIndex > 0;
                if (ImGui::Checkbox("Cloak", &hasCloak))
                    appearance.cloakIndex = hasCloak ? 1 : -1;
                if (hasCloak)
                {
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(60.0f);
                    ImGui::InputInt("##cloakIdx", &appearance.cloakIndex);
                    if (appearance.cloakIndex < 1) appearance.cloakIndex = 1;
                }

                const int maxBone = std::max(0, characterSystem.animation_bone_count() - 1);
                const bool showBoneSection = appearance.weaponType != WT::None || appearance.shieldType != WT::None;
                if (showBoneSection && maxBone > 0)
                {
                    ImGui::Separator();
                    ImGui::Text("Bone attach (0-%d)", maxBone);
                    if (appearance.weaponType != WT::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Wpn bone", &characterSystem.weaponBoneIndex);
                        characterSystem.weaponBoneIndex = std::clamp(characterSystem.weaponBoneIndex, 0, maxBone);
                        if (phoenix::character::weapon_type_dual_wield(appearance.weaponType))
                        {
                            ImGui::SetNextItemWidth(80.0f);
                            ImGui::InputInt("Dual bone", &characterSystem.dualWeaponBoneIndex);
                            characterSystem.dualWeaponBoneIndex = std::clamp(characterSystem.dualWeaponBoneIndex, 0, maxBone);
                            // Fine adjustment of the off-hand copy in its bone's
                            // local space (rotate, then translate).
                            ImGui::SetNextItemWidth(200.0f);
                            ImGui::DragFloat3("Dual offset", characterSystem.dualOffsetPos, 0.005f);
                            ImGui::SetNextItemWidth(200.0f);
                            ImGui::DragFloat3("Dual rot", characterSystem.dualOffsetRotDeg, 0.5f, -180.0f, 180.0f);
                        }
                    }
                    if (appearance.shieldType != WT::None)
                    {
                        ImGui::SetNextItemWidth(80.0f);
                        ImGui::InputInt("Shld bone", &characterSystem.shieldBoneIndex);
                        characterSystem.shieldBoneIndex = std::clamp(characterSystem.shieldBoneIndex, 0, maxBone);
                    }
                }
            }
        }
        else if (activeSection == Section::Vehicle)
        {
            if (!assetsReady)
            {
                ImGui::TextDisabled("Loading assets...");
            }
            else
            {
                ImGui::Checkbox("Mount", &appearance.mounted);
                if (appearance.mounted)
                {
                    static const char* mountClasses[] = { "hu", "de", "el", "vi" };
                    int classIdx = 0;
                    for (int i = 0; i < 4; ++i)
                        if (appearance.mountClass == mountClasses[i]) { classIdx = i; break; }
                    ImGui::SetNextItemWidth(80.0f);
                    if (ImGui::BeginCombo("Class", mountClasses[classIdx]))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const bool selected = i == classIdx;
                            if (ImGui::Selectable(mountClasses[i], selected))
                                appearance.mountClass = mountClasses[i];
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("Index", &appearance.mountIndex);
                    if (appearance.mountIndex < 0) appearance.mountIndex = 0;

                    // Always expose the seat bone — even mounts whose skeleton
                    // reports 0/1 bones must stay editable from the tool.
                    ImGui::SetNextItemWidth(80.0f);
                    ImGui::InputInt("Seat bone", &characterSystem.mountBoneIndex);
                    const int boneCount = characterSystem.mount_bone_count();
                    if (boneCount > 0)
                        characterSystem.mountBoneIndex = std::clamp(characterSystem.mountBoneIndex, 0, boneCount - 1);
                    else
                        characterSystem.mountBoneIndex = std::max(0, characterSystem.mountBoneIndex);
                }
            }
        }
        else if (activeSection == Section::Bots)
        {
            ImGui::Text("Bots: %d", static_cast<int>(botCount));
            if (botControlsAvailable)
            {
                if (ImGui::Button("Spawn 10", ImVec2(95.0f, 0.0f)))
                    result.botSpawnCount = 10;
                ImGui::SameLine();
                if (ImGui::Button("Spawn 100", ImVec2(95.0f, 0.0f)))
                    result.botSpawnCount = 100;
                if (ImGui::Button("Clear All", ImVec2(195.0f, 0.0f)))
                    result.clearBots = true;
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("View dist", &botViewDistance, 20.0f, 300.0f, "%.0f m");
                ImGui::Checkbox("Bot cast effects", &botEffectsEnabled);
            }
            else
            {
                ImGui::TextDisabled("Playable character required.");
            }
        }
        else if (activeSection == Section::NPCs)
        {
            static int selectedNpc = 0;
            ImGui::Text("Active: %d", static_cast<int>(npcActiveCount));
            if (!npcStatus.empty())
                ImGui::TextDisabled("%s", npcStatus.c_str());
            if (npcCatalog.empty())
            {
                ImGui::TextDisabled("No NPC catalog loaded.");
            }
            else
            {
                selectedNpc = std::clamp(selectedNpc, 0, static_cast<int>(npcCatalog.size()) - 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::BeginCombo("NPC", npcCatalog[static_cast<std::size_t>(selectedNpc)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(npcCatalog.size()); ++i)
                    {
                        const bool selected = selectedNpc == i;
                        if (ImGui::Selectable(npcCatalog[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedNpc = i;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                const auto spawnNpc = [&](int count) {
                    result.npcSpawnCatalogIndex = selectedNpc;
                    result.npcSpawnCount = count;
                };
                if (ImGui::Button("Spawn", ImVec2(95.0f, 0.0f)))    spawnNpc(1);
                ImGui::SameLine();
                if (ImGui::Button("Clear", ImVec2(95.0f, 0.0f)))    result.clearNpcs = true;
                if (ImGui::Button("Spawn 10", ImVec2(95.0f, 0.0f))) spawnNpc(10);
                ImGui::SameLine();
                if (ImGui::Button("Spawn 50", ImVec2(95.0f, 0.0f))) spawnNpc(50);
                if (ImGui::Button("Spawn 50 random", ImVec2(195.0f, 0.0f)))
                {
                    result.npcSpawnRandom = true;
                    result.npcSpawnCount = 50;
                }
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("Cull dist", &npcViewDistance, 20.0f, 300.0f, "%.0f m");
            }
        }
        else if (activeSection == Section::Monsters)
        {
            static int selectedMonster = 0;
            ImGui::Text("Active: %d", static_cast<int>(monsterActiveCount));
            if (!monsterStatus.empty())
                ImGui::TextDisabled("%s", monsterStatus.c_str());
            if (monsterCatalog.empty())
            {
                ImGui::TextDisabled("No monster catalog loaded.");
            }
            else
            {
                selectedMonster = std::clamp(selectedMonster, 0, static_cast<int>(monsterCatalog.size()) - 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::BeginCombo("Monster", monsterCatalog[static_cast<std::size_t>(selectedMonster)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(monsterCatalog.size()); ++i)
                    {
                        const bool selected = selectedMonster == i;
                        if (ImGui::Selectable(monsterCatalog[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedMonster = i;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                const auto spawnMonster = [&](int count) {
                    result.monsterSpawnCatalogIndex = selectedMonster;
                    result.monsterSpawnCount = count;
                };
                if (ImGui::Button("Spawn", ImVec2(95.0f, 0.0f)))    spawnMonster(1);
                ImGui::SameLine();
                if (ImGui::Button("Clear", ImVec2(95.0f, 0.0f)))    result.clearMonsters = true;
                if (ImGui::Button("Spawn 10", ImVec2(95.0f, 0.0f))) spawnMonster(10);
                ImGui::SameLine();
                if (ImGui::Button("Spawn 50", ImVec2(95.0f, 0.0f))) spawnMonster(50);
                if (ImGui::Button("Spawn 50 random", ImVec2(195.0f, 0.0f)))
                {
                    result.monsterSpawnRandom = true;
                    result.monsterSpawnCount = 50;
                }
                ImGui::SetNextItemWidth(180.0f);
                ImGui::SliderFloat("Cull dist", &monsterViewDistance, 20.0f, 300.0f, "%.0f m");
            }
        }
        else if (activeSection == Section::Animations)
        {
            struct AnimationChoice
            {
                std::string label;
                std::size_t index{};
            };

            std::vector<AnimationChoice> choices;
            const auto& data = characterSystem.character_data();
            const auto addChoice = [&](const char* label, std::size_t animationIndex, bool allowZero = false) {
                if ((!allowZero && animationIndex == 0) || animationIndex >= data.animations.size())
                    return;
                const auto& choice = data.animations[animationIndex];
                if (!choice.animation.parsed)
                    return;
                std::string text = label;
                if (!choice.name.empty())
                {
                    text += "  [";
                    text += choice.name;
                    text += "]";
                }
                choices.push_back({ std::move(text), animationIndex });
            };

            addChoice("Idle", data.idleAnimation, true);
            addChoice("Walk", data.walkAnimation);
            addChoice("Run", data.runAnimation);
            addChoice("Backstep", data.backAnimation);
            addChoice("Left step", data.leftAnimation);
            addChoice("Right step", data.rightAnimation);
            addChoice("Swim idle", data.swimIdleAnimation);
            addChoice("Swim", data.swimAnimation);
            addChoice("Jump", data.jumpAnimation);
            addChoice("Die", data.dieAnimation);
            addChoice("Sit down", data.sitDownAnimation);
            addChoice("Sit up", data.sitUpAnimation);
            addChoice("Sit", data.sitAnimation);
            addChoice("Dodge back", data.dodgeBackAnimation);
            addChoice("Dodge left", data.dodgeLeftAnimation);
            addChoice("Dodge right", data.dodgeRightAnimation);
            addChoice("Idle gesture 1", data.idle1Animation);
            addChoice("Idle gesture 2", data.idle2Animation);
            addChoice("Ladder", data.ladderAnimation);
            addChoice("Select", data.selectAnimation);
            addChoice("Vehicle run 1", data.vehicleRun1Animation);
            addChoice("Vehicle idle", data.vehicleIdleAnimation);
            addChoice("Vehicle run 2", data.vehicleRun2Animation);

            addChoice("2H ready", data.twoHandReadyAnimation);
            addChoice("2H attack 1", data.twoHandAttack1Animation);
            addChoice("2H attack 2", data.twoHandAttack2Animation);
            addChoice("2H attack 3", data.twoHandAttack3Animation);
            addChoice("2H attack 4", data.twoHandAttack4Animation);
            addChoice("2H damage", data.twoHandDamageAnimation);
            addChoice("2H run", data.twoHandRunAnimation);
            addChoice("Bow ready", data.bowReadyAnimation);
            addChoice("Bow attack", data.bowAttackAnimation);
            addChoice("Bow damage", data.bowDamageAnimation);
            addChoice("Bow run", data.bowRunAnimation);
            addChoice("1H ready", data.oneHandReadyAnimation);
            addChoice("1H attack 1", data.oneHandAttack1Animation);
            addChoice("1H attack 2", data.oneHandAttack2Animation);
            addChoice("1H attack 3", data.oneHandAttack3Animation);
            addChoice("1H attack 4", data.oneHandAttack4Animation);
            addChoice("1H damage", data.oneHandDamageAnimation);
            addChoice("1H run", data.oneHandRunAnimation);
            addChoice("Dual ready", data.dualReadyAnimation);
            addChoice("Dual attack 1", data.dualAttack1Animation);
            addChoice("Dual attack 2", data.dualAttack2Animation);
            addChoice("Dual attack 3", data.dualAttack3Animation);
            addChoice("Dual attack 4", data.dualAttack4Animation);
            addChoice("Dual damage", data.dualDamageAnimation);
            addChoice("Dual run", data.dualRunAnimation);
            addChoice("Spear ready", data.spearReadyAnimation);
            addChoice("Spear attack 1", data.spearAttack1Animation);
            addChoice("Spear attack 2", data.spearAttack2Animation);
            addChoice("Spear attack 3", data.spearAttack3Animation);
            addChoice("Spear attack 4", data.spearAttack4Animation);
            addChoice("Spear damage", data.spearDamageAnimation);
            addChoice("Spear run", data.spearRunAnimation);
            addChoice("Crossbow ready", data.crossbowReadyAnimation);
            addChoice("Crossbow attack", data.crossbowAttackAnimation);
            addChoice("Crossbow damage", data.crossbowDamageAnimation);
            addChoice("Crossbow run", data.crossbowRunAnimation);
            addChoice("Staff ready", data.staffReadyAnimation);
            addChoice("Staff attack 1", data.staffAttack1Animation);
            addChoice("Staff attack 2", data.staffAttack2Animation);
            addChoice("Staff damage", data.staffDamageAnimation);
            addChoice("Staff run", data.staffRunAnimation);
            addChoice("Rev dagger ready", data.revDaggerReadyAnimation);
            addChoice("Rev dagger attack 1", data.revDaggerAttack1Animation);
            addChoice("Rev dagger attack 2", data.revDaggerAttack2Animation);
            addChoice("Rev dagger attack 3", data.revDaggerAttack3Animation);
            addChoice("Rev dagger attack 4", data.revDaggerAttack4Animation);
            addChoice("Rev dagger damage", data.revDaggerDamageAnimation);
            addChoice("Rev dagger run", data.revDaggerRunAnimation);
            addChoice("Knuckle ready", data.knuckleReadyAnimation);
            addChoice("Knuckle attack 1", data.knuckleAttack1Animation);
            addChoice("Knuckle attack 2", data.knuckleAttack2Animation);
            addChoice("Knuckle attack 3", data.knuckleAttack3Animation);
            addChoice("Knuckle attack 4", data.knuckleAttack4Animation);
            addChoice("Knuckle damage", data.knuckleDamageAnimation);
            addChoice("Knuckle run", data.knuckleRunAnimation);
            addChoice("Dagger ready", data.daggerReadyAnimation);
            addChoice("Dagger attack 1", data.daggerAttack1Animation);
            addChoice("Dagger attack 2", data.daggerAttack2Animation);
            addChoice("Dagger attack 3", data.daggerAttack3Animation);
            addChoice("Dagger attack 4", data.daggerAttack4Animation);
            addChoice("Dagger damage", data.daggerDamageAnimation);
            addChoice("Dagger run", data.daggerRunAnimation);

            addChoice("Magic ready 1", data.magicReady1Animation);
            addChoice("Magic cast 1", data.magicCast1Animation);
            addChoice("Magic attack 1", data.magicAttack1Animation);
            addChoice("Magic ready 2", data.magicReady2Animation);
            addChoice("Magic cast 2", data.magicCast2Animation);
            addChoice("Magic attack 2", data.magicAttack2Animation);
            addChoice("Buff ready 1", data.buffReady1Animation);
            addChoice("Buff cast 1", data.buffCast1Animation);
            addChoice("Buff attack 1", data.buffAttack1Animation);
            addChoice("Buff ready 2", data.buffReady2Animation);
            addChoice("Buff cast 2", data.buffCast2Animation);
            addChoice("Buff attack 2", data.buffAttack2Animation);
            addChoice("Buff ready 3", data.buffReady3Animation);
            addChoice("Buff cast 3", data.buffCast3Animation);
            addChoice("Buff attack 3", data.buffAttack3Animation);
            addChoice("Skill 1", data.skill1Animation);
            addChoice("Skill 2", data.skill2Animation);
            addChoice("Skill 3", data.skill3Animation);
            addChoice("Skill 4", data.skill4Animation);
            addChoice("Skill 5", data.skill5Animation);
            addChoice("Skill 6", data.skill6Animation);
            addChoice("Skill 7", data.skill7Animation);
            addChoice("Skill 8", data.skill8Animation);
            addChoice("Skill 9", data.skill9Animation);
            addChoice("Skill 10", data.skill10Animation);
            addChoice("Skill 11", data.skill11Animation);
            addChoice("Stun", data.stunAnimation);
            addChoice("Emote 1", data.emote1Animation);
            addChoice("Emote 2", data.emote2Animation);
            addChoice("Emote 3", data.emote3Animation);
            addChoice("Emote 4", data.emote4Animation);
            addChoice("Emote 5", data.emote5Animation);
            addChoice("Emote 6", data.emote6Animation);
            addChoice("Emote 7", data.emote7Animation);
            addChoice("Emote 8", data.emote8Animation);
            addChoice("Emote 9", data.emote9Animation);
            addChoice("Emote 10", data.emote10Animation);

            static int selectedAnimation = 0;
            if (choices.empty())
            {
                ImGui::TextDisabled("No mapped animations loaded.");
            }
            else
            {
                selectedAnimation = std::clamp(selectedAnimation, 0, static_cast<int>(choices.size()) - 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::BeginCombo("Animation", choices[static_cast<std::size_t>(selectedAnimation)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(choices.size()); ++i)
                    {
                        const bool selected = selectedAnimation == i;
                        if (ImGui::Selectable(choices[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedAnimation = i;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Play"))
                    result.animationTriggered = choices[static_cast<std::size_t>(selectedAnimation)].index;
            }
        }
        else if (activeSection == Section::Effects)
        {
            ImGui::Text("Active: %d", static_cast<int>(effectActiveCount));
            if (effectFileNames.empty())
            {
                ImGui::TextDisabled("No .eft/.ef2/.ef3 files found under data/effects.");
            }
            else
            {
                selectedEffectFileIndex = std::clamp(selectedEffectFileIndex, 0, static_cast<int>(effectFileNames.size()) - 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::BeginCombo("File", effectFileNames[static_cast<std::size_t>(selectedEffectFileIndex)].c_str()))
                {
                    for (int i = 0; i < static_cast<int>(effectFileNames.size()); ++i)
                    {
                        const bool selected = selectedEffectFileIndex == i;
                        if (ImGui::Selectable(effectFileNames[static_cast<std::size_t>(i)].c_str(), selected))
                            selectedEffectFileIndex = i;
                        if (selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                if (effectSequenceNames.empty())
                {
                    ImGui::TextDisabled("This file has no effect sequences.");
                }
                else
                {
                    selectedEffectSequenceIndex = std::clamp(
                        selectedEffectSequenceIndex, 0, static_cast<int>(effectSequenceNames.size()) - 1);
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::BeginCombo("Effect", effectSequenceNames[static_cast<std::size_t>(selectedEffectSequenceIndex)].c_str()))
                    {
                        for (int i = 0; i < static_cast<int>(effectSequenceNames.size()); ++i)
                        {
                            const bool selected = selectedEffectSequenceIndex == i;
                            if (ImGui::Selectable(effectSequenceNames[static_cast<std::size_t>(i)].c_str(), selected))
                                selectedEffectSequenceIndex = i;
                            if (selected)
                                ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::SliderFloat("Y offset", &effectSpawnYOffset, -50.0f, 300.0f, "%.0f");
                    ImGui::TextDisabled("Spawns at the character's position, offset on Y.");
                    if (ImGui::Button("Spawn (stays)", ImVec2(140.0f, 0.0f)))
                    {
                        result.effectSpawnRequested = true;
                        result.effectSpawnOneShot = false;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Spawn (one-shot)", ImVec2(150.0f, 0.0f)))
                    {
                        result.effectSpawnRequested = true;
                        result.effectSpawnOneShot = true;
                    }
                }
                if (ImGui::Button("Clear All", ImVec2(295.0f, 0.0f)))
                    result.clearEffects = true;

                ImGui::Separator();
                ImGui::TextDisabled("Diagnostic: spawn one raw component directly");
                ImGui::SetNextItemWidth(100.0f);
                ImGui::InputInt("Component #", &effectComponentIndex);
                effectComponentIndex = std::max(0, effectComponentIndex);
                if (ImGui::Button("Spawn component", ImVec2(295.0f, 0.0f)))
                    result.effectComponentSpawnRequested = true;
            }
        }
        result.characterChanged = selectedCharacterOption != prevCharOption
            || appearance.raceFolder != prevAppearance.raceFolder
            || appearance.prefix != prevAppearance.prefix
            || appearance.upperIndex != prevAppearance.upperIndex
            || appearance.lowerIndex != prevAppearance.lowerIndex
            || appearance.handIndex != prevAppearance.handIndex
            || appearance.footIndex != prevAppearance.footIndex
            || appearance.helmetIndex != prevAppearance.helmetIndex
            || appearance.faceIndex != prevAppearance.faceIndex
            || appearance.hairIndex != prevAppearance.hairIndex
            || appearance.helmetVisible != prevAppearance.helmetVisible
            || appearance.weaponType != prevAppearance.weaponType
            || appearance.weaponIndex != prevAppearance.weaponIndex
            || appearance.shieldType != prevAppearance.shieldType
            || appearance.shieldIndex != prevAppearance.shieldIndex
            || appearance.cloakIndex != prevAppearance.cloakIndex
            || appearance.mounted != prevAppearance.mounted
            || appearance.mountClass != prevAppearance.mountClass
            || appearance.mountIndex != prevAppearance.mountIndex;

        ImGui::End();
        return result;
    }
}
