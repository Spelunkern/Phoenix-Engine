#include "ui/editor_panel.h"
#include "ui/app_settings.h"
#include "ui/cpu_profiler.h"

#include "ui/phoenix_ui.h"

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
        bool& vsyncEnabled,
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
            Controls,
        };
        static Section activeSection = Section::Map;

        px::SetNextWindowPos(px::Vec2(8.0f, 8.0f), px::FirstUseEver);
        px::SetNextWindowSize(px::Vec2(388.0f, 0.0f), px::FirstUseEver);
        if (!px::Begin("Phoenix Client", nullptr, px::AlwaysAutoResize))
        {
            px::End();
            return result;
        }

        auto sectionButton = [&](Section section, const char* label) {
            const bool selected = activeSection == section;
            if (selected)
                px::PushStyleColor(px::ButtonColor, px::GetStyleColor(px::ButtonActiveColor));
            if (px::Button(label, px::Vec2(68.0f, 0.0f)))
                activeSection = section;
            if (selected)
                px::PopStyleColor();
        };

        sectionButton(Section::Map, "Map##nav"); px::SameLine();
        sectionButton(Section::Graphics, "Graphics##nav"); px::SameLine();
        sectionButton(Section::Sound, "Sound##nav"); px::SameLine();
        sectionButton(Section::Animations, "Animations##nav"); px::SameLine();
        sectionButton(Section::Effects, "Effects##nav");
        sectionButton(Section::Character, "Character##nav"); px::SameLine();
        sectionButton(Section::Vehicle, "Vehicle##nav"); px::SameLine();
        sectionButton(Section::Bots, "Bots##nav"); px::SameLine();
        sectionButton(Section::NPCs, "NPCs##nav"); px::SameLine();
        sectionButton(Section::Monsters, "Monsters##nav");
        sectionButton(Section::Controls, "Controls##nav");
        px::Separator();

        if (activeSection == Section::Map)
        {
            const auto& maps = runtime.world_map_names();
            if (!maps.empty())
            {
                selectedMapIndex = std::clamp(selectedMapIndex, 0, static_cast<int>(maps.size() - 1));
                px::SetNextItemWidth(200.0f);
                if (px::BeginCombo("Map##mapCombo", maps[static_cast<std::size_t>(selectedMapIndex)].c_str()))
                {
                    for (std::size_t i = 0; i < maps.size(); ++i)
                    {
                        if (px::Selectable(maps[i].c_str(), selectedMapIndex == static_cast<int>(i)))
                            selectedMapIndex = static_cast<int>(i);
                    }
                    px::EndCombo();
                }
                px::SameLine();
                result.loadRequested = px::Button("Load");
            }

            const auto previousFog = fogEnabled;
            px::Checkbox("Fog", &fogEnabled);
            if (fogEnabled != previousFog)
                apply_renderer_fog(renderer, runtime, fogEnabled, viewDistance, weatherMode);

            const auto previousViewDistance = viewDistance;
            px::SetNextItemWidth(220.0f);
            px::SliderFloat("Fog distance", &viewDistance, 100.0f, 2500.0f, "%.0f");
            result.viewDistanceChanged = std::abs(previousViewDistance - viewDistance) > 1.0f;

            const WeatherMode previousWeatherMode = weatherMode;
            const char* weatherItems[] = { "Default", "Clear day", "Dawn", "Afternoon",
                "Sunset", "Dusk", "Night", "Overcast", "Snow", "Storm" };
            int weatherIndex = static_cast<int>(weatherMode);
            px::SetNextItemWidth(180.0f);
            if (px::Combo("Sky", &weatherIndex, weatherItems, static_cast<int>(std::size(weatherItems))))
                weatherMode = static_cast<WeatherMode>(std::clamp(
                    weatherIndex, 0, static_cast<int>(std::size(weatherItems)) - 1));
            result.weatherChanged = weatherMode != previousWeatherMode;
        }
        else if (activeSection == Section::Controls)
        {
            px::TextUnformatted("Essential keys");
            px::Separator();
            px::BulletText("P   Character / free camera mode");
            px::BulletText("W A S D   Move");
            px::BulletText("Mouse wheel   Camera zoom");
            px::BulletText("Left mouse drag   Orbit camera");
            px::BulletText("Right mouse drag   Turn camera and character");
            px::BulletText("Space   Jump");
            px::BulletText("Shift   Walk (running is default)");
            px::BulletText("C   Sit / stand");
            px::Spacing();
            px::TextDisabled("Free camera: Q/E move vertically; Shift accelerates.");
        }
        else if (activeSection == Section::Graphics)
        {
            if (px::Checkbox("World shadows", &worldShadows))
            {
                renderer.set_shadows_enabled(worldShadows);
                flush_app_settings();
            }

            if (antialiasingAvailable)
            {
                if (px::Checkbox("Anti-aliasing", &antialiasingEnabled))
                {
                    renderer.set_antialiasing_enabled(antialiasingEnabled);
                    flush_app_settings();
                }
            }
            else
            {
                px::TextDisabled("Anti-aliasing unavailable");
            }

            if (px::Checkbox("VSync", &vsyncEnabled))
            {
                if (!renderer.set_vsync_enabled(vsyncEnabled))
                    vsyncEnabled = !vsyncEnabled;
                flush_app_settings();
            }

            const char* caps[] = { "Off", "30", "60", "75", "90", "120", "144", "165", "240", "360" };
            px::SetNextItemWidth(110.0f);
            if (px::Combo("FPS cap", &fpsCapIndex, caps, static_cast<int>(std::size(caps))))
                flush_app_settings();
        }
        else if (activeSection == Section::Sound)
        {
            px::Checkbox("Play Sounds", &playMapSounds);
            px::Checkbox("Play Music", &playMapMusic);
            px::SetNextItemWidth(220.0f);
            px::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f, "%.2f");
        }
        else if (activeSection == Section::Character)
        {
            if (characterOptions.empty())
            {
                px::TextDisabled("No character models found.");
            }
            else if (!assetsReady)
            {
                px::TextDisabled("Loading assets...");
            }
            else
            {
                selectedCharacterOption = std::clamp(selectedCharacterOption, 0, static_cast<int>(characterOptions.size() - 1));
                const auto& selected = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
                px::SetNextItemWidth(190.0f);
                if (px::BeginCombo("Model", selected.label.c_str()))
                {
                    for (std::size_t i = 0; i < characterOptions.size(); ++i)
                    {
                        const bool isSelected = selectedCharacterOption == static_cast<int>(i);
                        if (px::Selectable(characterOptions[i].label.c_str(), isSelected))
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
                            px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }

                const auto& current = characterOptions[static_cast<std::size_t>(selectedCharacterOption)];
                appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
                appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
                appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
                appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
                appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
                appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
                appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);

                px::Checkbox("Helmet", &appearance.helmetVisible);
                if (appearance.helmetVisible)
                {
                    px::SameLine();
                    px::SetNextItemWidth(80.0f);
                    px::InputInt("##helmet", &appearance.helmetIndex);
                    appearance.helmetIndex = nearest_available(appearance.helmetIndex, current.helmetIndices);
                }
                px::SetNextItemWidth(80.0f); px::InputInt("Upper", &appearance.upperIndex);
                appearance.upperIndex = nearest_available(appearance.upperIndex, current.upperIndices);
                px::SetNextItemWidth(80.0f); px::InputInt("Lower", &appearance.lowerIndex);
                appearance.lowerIndex = nearest_available(appearance.lowerIndex, current.lowerIndices);
                px::SetNextItemWidth(80.0f); px::InputInt("Gloves", &appearance.handIndex);
                appearance.handIndex = nearest_available(appearance.handIndex, current.handIndices);
                px::SetNextItemWidth(80.0f); px::InputInt("Boots", &appearance.footIndex);
                appearance.footIndex = nearest_available(appearance.footIndex, current.footIndices);
                px::SetNextItemWidth(80.0f); px::InputInt("Face", &appearance.faceIndex);
                appearance.faceIndex = nearest_available(appearance.faceIndex, current.faceIndices);
                if (!appearance.helmetVisible)
                {
                    px::SetNextItemWidth(80.0f); px::InputInt("Hair", &appearance.hairIndex);
                    appearance.hairIndex = nearest_available(appearance.hairIndex, current.hairIndices);
                }

                px::Separator();
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
                px::SetNextItemWidth(140.0f);
                if (px::BeginCombo("Weapon", currentWeaponLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(weaponLabels)); ++i)
                    {
                        const bool isSelected = i == currentWeaponIdx;
                        if (px::Selectable(weaponLabels[i].label, isSelected))
                        {
                            appearance.weaponType = weaponLabels[i].type;
                            if (appearance.weaponType == WT::None) appearance.weaponIndex = -1;
                            else if (appearance.weaponIndex < 0) appearance.weaponIndex = 1;
                        }
                        if (isSelected) px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }
                if (appearance.weaponType != WT::None)
                {
                    px::SameLine();
                    px::SetNextItemWidth(60.0f);
                    px::InputInt("##weapIdx", &appearance.weaponIndex);
                    if (appearance.weaponIndex < 1) appearance.weaponIndex = 1;
                }

                const char* currentShieldLabel = "None";
                int currentShieldIdx = 0;
                for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    if (shieldLabels[i].type == appearance.shieldType) { currentShieldLabel = shieldLabels[i].label; currentShieldIdx = i; break; }
                px::SetNextItemWidth(140.0f);
                if (px::BeginCombo("Shield", currentShieldLabel))
                {
                    for (int i = 0; i < static_cast<int>(std::size(shieldLabels)); ++i)
                    {
                        const bool isSelected = i == currentShieldIdx;
                        if (px::Selectable(shieldLabels[i].label, isSelected))
                        {
                            appearance.shieldType = shieldLabels[i].type;
                            if (appearance.shieldType == WT::None) appearance.shieldIndex = -1;
                            else if (appearance.shieldIndex < 0) appearance.shieldIndex = 1;
                        }
                        if (isSelected) px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }
                if (appearance.shieldType != WT::None)
                {
                    px::SameLine();
                    px::SetNextItemWidth(60.0f);
                    px::InputInt("##shldIdx", &appearance.shieldIndex);
                    if (appearance.shieldIndex < 1) appearance.shieldIndex = 1;
                }

                px::Separator();
                bool hasCloak = appearance.cloakIndex > 0;
                if (px::Checkbox("Cloak", &hasCloak))
                    appearance.cloakIndex = hasCloak ? 1 : -1;
                if (hasCloak)
                {
                    px::SameLine();
                    px::SetNextItemWidth(60.0f);
                    px::InputInt("##cloakIdx", &appearance.cloakIndex);
                    if (appearance.cloakIndex < 1) appearance.cloakIndex = 1;
                }

                const int maxBone = std::max(0, characterSystem.animation_bone_count() - 1);
                const bool showBoneSection = appearance.weaponType != WT::None || appearance.shieldType != WT::None;
                if (showBoneSection && maxBone > 0)
                {
                    px::Separator();
                    px::Text("Bone attach (0-%d)", maxBone);
                    if (appearance.weaponType != WT::None)
                    {
                        px::SetNextItemWidth(80.0f);
                        px::InputInt("Wpn bone", &characterSystem.weaponBoneIndex);
                        characterSystem.weaponBoneIndex = std::clamp(characterSystem.weaponBoneIndex, 0, maxBone);
                        if (phoenix::character::weapon_type_dual_wield(appearance.weaponType))
                        {
                            px::SetNextItemWidth(80.0f);
                            px::InputInt("Dual bone", &characterSystem.dualWeaponBoneIndex);
                            characterSystem.dualWeaponBoneIndex = std::clamp(characterSystem.dualWeaponBoneIndex, 0, maxBone);
                            // Fine adjustment of the off-hand copy in its bone's
                            // local space (rotate, then translate).
                            px::SetNextItemWidth(200.0f);
                            px::DragFloat3("Dual offset", characterSystem.dualOffsetPos, 0.005f);
                            px::SetNextItemWidth(200.0f);
                            px::DragFloat3("Dual rot", characterSystem.dualOffsetRotDeg, 0.5f, -180.0f, 180.0f);
                        }
                    }
                    if (appearance.shieldType != WT::None)
                    {
                        px::SetNextItemWidth(80.0f);
                        px::InputInt("Shld bone", &characterSystem.shieldBoneIndex);
                        characterSystem.shieldBoneIndex = std::clamp(characterSystem.shieldBoneIndex, 0, maxBone);
                    }
                }
            }
        }
        else if (activeSection == Section::Vehicle)
        {
            if (!assetsReady)
            {
                px::TextDisabled("Loading assets...");
            }
            else
            {
                px::Checkbox("Mount", &appearance.mounted);
                if (appearance.mounted)
                {
                    static const char* mountClasses[] = { "hu", "de", "el", "vi" };
                    int classIdx = 0;
                    for (int i = 0; i < 4; ++i)
                        if (appearance.mountClass == mountClasses[i]) { classIdx = i; break; }
                    px::SetNextItemWidth(80.0f);
                    if (px::BeginCombo("Class", mountClasses[classIdx]))
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            const bool selected = i == classIdx;
                            if (px::Selectable(mountClasses[i], selected))
                                appearance.mountClass = mountClasses[i];
                            if (selected) px::SetItemDefaultFocus();
                        }
                        px::EndCombo();
                    }
                    px::SetNextItemWidth(80.0f);
                    px::InputInt("Index", &appearance.mountIndex);
                    if (appearance.mountIndex < 0) appearance.mountIndex = 0;

                    // Always expose the seat bone — even mounts whose skeleton
                    // reports 0/1 bones must stay editable from the tool.
                    px::SetNextItemWidth(80.0f);
                    px::InputInt("Seat bone", &characterSystem.mountBoneIndex);
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
            px::Text("Bots: %d", static_cast<int>(botCount));
            if (botControlsAvailable)
            {
                if (px::Button("Spawn 10", px::Vec2(95.0f, 0.0f)))
                    result.botSpawnCount = 10;
                px::SameLine();
                if (px::Button("Spawn 100", px::Vec2(95.0f, 0.0f)))
                    result.botSpawnCount = 100;
                if (px::Button("Clear All", px::Vec2(195.0f, 0.0f)))
                    result.clearBots = true;
                px::SetNextItemWidth(180.0f);
                px::SliderFloat("View dist", &botViewDistance, 20.0f, 300.0f, "%.0f m");
                px::Checkbox("Bot cast effects", &botEffectsEnabled);
            }
            else
            {
                px::TextDisabled("Playable character required.");
            }
        }
        else if (activeSection == Section::NPCs)
        {
            static int selectedNpc = 0;
            px::Text("Active: %d", static_cast<int>(npcActiveCount));
            if (!npcStatus.empty())
                px::TextDisabled("%s", npcStatus.c_str());
            if (npcCatalog.empty())
            {
                px::TextDisabled("No NPC catalog loaded.");
            }
            else
            {
                selectedNpc = std::clamp(selectedNpc, 0, static_cast<int>(npcCatalog.size()) - 1);
                px::SetNextItemWidth(240.0f);
                if (px::BeginCombo("NPC", npcCatalog[static_cast<std::size_t>(selectedNpc)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(npcCatalog.size()); ++i)
                    {
                        const bool selected = selectedNpc == i;
                        if (px::Selectable(npcCatalog[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedNpc = i;
                        if (selected)
                            px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }
                const auto spawnNpc = [&](int count) {
                    result.npcSpawnCatalogIndex = selectedNpc;
                    result.npcSpawnCount = count;
                };
                if (px::Button("Spawn", px::Vec2(95.0f, 0.0f)))    spawnNpc(1);
                px::SameLine();
                if (px::Button("Clear", px::Vec2(95.0f, 0.0f)))    result.clearNpcs = true;
                if (px::Button("Spawn 10", px::Vec2(95.0f, 0.0f))) spawnNpc(10);
                px::SameLine();
                if (px::Button("Spawn 50", px::Vec2(95.0f, 0.0f))) spawnNpc(50);
                if (px::Button("Spawn 50 random", px::Vec2(195.0f, 0.0f)))
                {
                    result.npcSpawnRandom = true;
                    result.npcSpawnCount = 50;
                }
                px::SetNextItemWidth(180.0f);
                px::SliderFloat("Cull dist", &npcViewDistance, 20.0f, 300.0f, "%.0f m");
            }
        }
        else if (activeSection == Section::Monsters)
        {
            static int selectedMonster = 0;
            px::Text("Active: %d", static_cast<int>(monsterActiveCount));
            if (!monsterStatus.empty())
                px::TextDisabled("%s", monsterStatus.c_str());
            if (monsterCatalog.empty())
            {
                px::TextDisabled("No monster catalog loaded.");
            }
            else
            {
                selectedMonster = std::clamp(selectedMonster, 0, static_cast<int>(monsterCatalog.size()) - 1);
                px::SetNextItemWidth(240.0f);
                if (px::BeginCombo("Monster", monsterCatalog[static_cast<std::size_t>(selectedMonster)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(monsterCatalog.size()); ++i)
                    {
                        const bool selected = selectedMonster == i;
                        if (px::Selectable(monsterCatalog[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedMonster = i;
                        if (selected)
                            px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }
                const auto spawnMonster = [&](int count) {
                    result.monsterSpawnCatalogIndex = selectedMonster;
                    result.monsterSpawnCount = count;
                };
                if (px::Button("Spawn", px::Vec2(95.0f, 0.0f)))    spawnMonster(1);
                px::SameLine();
                if (px::Button("Clear", px::Vec2(95.0f, 0.0f)))    result.clearMonsters = true;
                if (px::Button("Spawn 10", px::Vec2(95.0f, 0.0f))) spawnMonster(10);
                px::SameLine();
                if (px::Button("Spawn 50", px::Vec2(95.0f, 0.0f))) spawnMonster(50);
                if (px::Button("Spawn 50 random", px::Vec2(195.0f, 0.0f)))
                {
                    result.monsterSpawnRandom = true;
                    result.monsterSpawnCount = 50;
                }
                px::SetNextItemWidth(180.0f);
                px::SliderFloat("Cull dist", &monsterViewDistance, 20.0f, 300.0f, "%.0f m");
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
                px::TextDisabled("No mapped animations loaded.");
            }
            else
            {
                selectedAnimation = std::clamp(selectedAnimation, 0, static_cast<int>(choices.size()) - 1);
                px::SetNextItemWidth(240.0f);
                if (px::BeginCombo("Animation", choices[static_cast<std::size_t>(selectedAnimation)].label.c_str()))
                {
                    for (int i = 0; i < static_cast<int>(choices.size()); ++i)
                    {
                        const bool selected = selectedAnimation == i;
                        if (px::Selectable(choices[static_cast<std::size_t>(i)].label.c_str(), selected))
                            selectedAnimation = i;
                        if (selected)
                            px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }
                if (px::Button("Play"))
                    result.animationTriggered = choices[static_cast<std::size_t>(selectedAnimation)].index;
            }
        }
        else if (activeSection == Section::Effects)
        {
            px::Text("Active: %d", static_cast<int>(effectActiveCount));
            if (effectFileNames.empty())
            {
                px::TextDisabled("No .eft/.ef2/.ef3 files found under data/effects.");
            }
            else
            {
                selectedEffectFileIndex = std::clamp(selectedEffectFileIndex, 0, static_cast<int>(effectFileNames.size()) - 1);
                px::SetNextItemWidth(240.0f);
                if (px::BeginCombo("File", effectFileNames[static_cast<std::size_t>(selectedEffectFileIndex)].c_str()))
                {
                    for (int i = 0; i < static_cast<int>(effectFileNames.size()); ++i)
                    {
                        const bool selected = selectedEffectFileIndex == i;
                        if (px::Selectable(effectFileNames[static_cast<std::size_t>(i)].c_str(), selected))
                            selectedEffectFileIndex = i;
                        if (selected)
                            px::SetItemDefaultFocus();
                    }
                    px::EndCombo();
                }

                if (effectSequenceNames.empty())
                {
                    px::TextDisabled("This file has no effect sequences.");
                }
                else
                {
                    selectedEffectSequenceIndex = std::clamp(
                        selectedEffectSequenceIndex, 0, static_cast<int>(effectSequenceNames.size()) - 1);
                    px::SetNextItemWidth(240.0f);
                    if (px::BeginCombo("Effect", effectSequenceNames[static_cast<std::size_t>(selectedEffectSequenceIndex)].c_str()))
                    {
                        for (int i = 0; i < static_cast<int>(effectSequenceNames.size()); ++i)
                        {
                            const bool selected = selectedEffectSequenceIndex == i;
                            if (px::Selectable(effectSequenceNames[static_cast<std::size_t>(i)].c_str(), selected))
                                selectedEffectSequenceIndex = i;
                            if (selected)
                                px::SetItemDefaultFocus();
                        }
                        px::EndCombo();
                    }
                    px::SetNextItemWidth(150.0f);
                    px::SliderFloat("Y offset", &effectSpawnYOffset, -50.0f, 300.0f, "%.0f");
                    px::TextDisabled("Spawns at the character's position, offset on Y.");
                    if (px::Button("Spawn (stays)", px::Vec2(140.0f, 0.0f)))
                    {
                        result.effectSpawnRequested = true;
                        result.effectSpawnOneShot = false;
                    }
                    px::SameLine();
                    if (px::Button("Spawn (one-shot)", px::Vec2(150.0f, 0.0f)))
                    {
                        result.effectSpawnRequested = true;
                        result.effectSpawnOneShot = true;
                    }
                }
                if (px::Button("Clear All", px::Vec2(295.0f, 0.0f)))
                    result.clearEffects = true;

                px::Separator();
                px::TextDisabled("Diagnostic: spawn one raw component directly");
                px::SetNextItemWidth(100.0f);
                px::InputInt("Component #", &effectComponentIndex);
                effectComponentIndex = std::max(0, effectComponentIndex);
                if (px::Button("Spawn component", px::Vec2(295.0f, 0.0f)))
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

        px::End();
        return result;
    }
}
