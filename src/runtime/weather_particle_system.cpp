#include "runtime/weather_particle_system.h"

#include <algorithm>
#include <cmath>

namespace phoenix::runtime
{
    namespace
    {
        using phoenix::renderer::TerrainVertex;
        using phoenix::renderer::EffectParticleInstance;
        using phoenix::renderer::EffectParticleBatch;

        // effect_particle.frag's texture-less sentinels — see that shader's
        // comment. Never collide with a real .eft texture layer.
        constexpr std::uint32_t kSoftStreakLayer = 0xFFFFFFFDu;
        constexpr std::uint32_t kSoftCircleLayer = 0xFFFFFFFEu;

        constexpr float kVolumeRadius = 45.0f;
        constexpr float kSpawnTop = 30.0f;
        constexpr float kDespawnBottom = -8.0f;
        constexpr float kIntensityScale[3]{ 0.35f, 0.65f, 1.0f };

        constexpr std::uint32_t kRainPoolSize = 2600u;
        // A slight world-space wind so rain reads as falling rain instead of
        // vertical pillars — both the per-frame motion and the streak's long
        // axis follow this same vector (see kRainStreak* below).
        constexpr float kRainVelocity[3]{ 2.2f, -11.0f, 0.9f };
        constexpr float kRainWidth = 0.05f;
        constexpr float kRainLength = 1.3f;
        constexpr float kRainAlpha = 0.4f;
        constexpr float kRainColor[3]{ 0.75f, 0.82f, 0.92f };

        constexpr std::uint32_t kSnowPoolSize = 1600u;
        constexpr float kSnowFallSpeed = 2.0f;
        constexpr float kSnowSize = 0.22f;
        constexpr float kSnowAlpha = 0.85f;
        constexpr float kSnowColor[3]{ 0.95f, 0.97f, 1.0f };
        constexpr float kSnowSwayAmplitude = 1.2f;
        constexpr float kSnowSwaySpeed = 0.6f;

        float fract(float v) { return v - std::floor(v); }
        float random01(float seed) { return fract(std::sin(seed * 12.9898f) * 43758.5453f); }

        void normalize3(float v[3], const float fallback[3])
        {
            const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (lenSq < 0.000001f)
            {
                v[0] = fallback[0]; v[1] = fallback[1]; v[2] = fallback[2];
                return;
            }
            const float inv = 1.0f / std::sqrt(lenSq);
            v[0] *= inv; v[1] *= inv; v[2] *= inv;
        }

        void cross3(const float a[3], const float b[3], float out[3])
        {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }
    }

    void WeatherParticleSystem::respawn(Particle& particle, float seed) const
    {
        const float rx = random01(seed) * 2.0f - 1.0f;
        const float rz = random01(seed + 91.7f) * 2.0f - 1.0f;
        const float rh = random01(seed + 173.3f);
        particle.offset[0] = rx * kVolumeRadius;
        particle.offset[1] = kDespawnBottom + (kSpawnTop - kDespawnBottom) * (0.5f + 0.5f * rh);
        particle.offset[2] = rz * kVolumeRadius;
    }

    void WeatherParticleSystem::set_weather(int kind, WeatherIntensity intensity)
    {
        kind_ = kind;
        intensity_ = intensity;

        if (rainParticles_.empty() && snowParticles_.empty())
        {
            rainParticles_.resize(kRainPoolSize);
            snowParticles_.resize(kSnowPoolSize);
            for (std::uint32_t i = 0; i < kRainPoolSize; ++i)
            {
                const float seed = static_cast<float>(seedCounter_++) * 37.13f;
                respawn(rainParticles_[i], seed);
                rainParticles_[i].sizeScale = 0.8f + random01(seed + 251.1f) * 0.4f;
            }
            for (std::uint32_t i = 0; i < kSnowPoolSize; ++i)
            {
                const float seed = static_cast<float>(seedCounter_++) * 37.13f;
                respawn(snowParticles_[i], seed);
                snowParticles_[i].sizeScale = 0.7f + random01(seed + 251.1f) * 0.6f;
            }
        }

        const float scale = kIntensityScale[static_cast<int>(intensity)];
        rainActiveCount_ = static_cast<std::uint32_t>(std::ceil(static_cast<float>(kRainPoolSize) * scale));
        snowActiveCount_ = static_cast<std::uint32_t>(std::ceil(static_cast<float>(kSnowPoolSize) * scale));
    }

    void WeatherParticleSystem::update(float deltaSeconds,
        const float cameraPosition[3],
        const float cameraRight[3], const float cameraUp[3], const float cameraForward[3])
    {
        vertices_.clear();
        indices_.clear();
        instances_.clear();
        batches_.clear();

        if (kind_ != 1 && kind_ != 2)
            return;

        const float dt = std::clamp(deltaSeconds, 0.0f, 0.1f);
        const bool isRain = kind_ == 1;

        // Rain streaks are stretched along their own (slightly windblown)
        // fall direction, not pure world-up, so they read as falling rain
        // instead of vertical pillars; snow stays a plain camera-facing
        // billboard. Both share one approximate view direction across the
        // whole volume (fine at this scale).
        float fallDir[3]{ kRainVelocity[0], kRainVelocity[1], kRainVelocity[2] };
        const float worldUp[3]{ 0.0f, 1.0f, 0.0f };
        normalize3(fallDir, worldUp);
        float streakRight[3];
        cross3(fallDir, cameraForward, streakRight);
        normalize3(streakRight, cameraRight);

        std::vector<EffectParticleInstance> items;
        items.reserve(isRain ? rainActiveCount_ : snowActiveCount_);

        if (isRain)
        {
            for (std::uint32_t i = 0; i < rainActiveCount_; ++i)
            {
                auto& particle = rainParticles_[i];
                particle.offset[0] += kRainVelocity[0] * dt;
                particle.offset[1] += kRainVelocity[1] * dt;
                particle.offset[2] += kRainVelocity[2] * dt;
                if (particle.offset[1] < kDespawnBottom)
                    respawn(particle, static_cast<float>(seedCounter_++) * 53.7f);

                const float widthScale = kRainWidth * particle.sizeScale;
                const float lengthScale = kRainLength * particle.sizeScale;
                EffectParticleInstance instance{};
                instance.color[0] = kRainColor[0]; instance.color[1] = kRainColor[1]; instance.color[2] = kRainColor[2];
                instance.color[3] = kRainAlpha;
                instance.position[0] = cameraPosition[0] + particle.offset[0];
                instance.position[1] = cameraPosition[1] + particle.offset[1];
                instance.position[2] = cameraPosition[2] + particle.offset[2];
                instance.forward[0] = cameraForward[0]; instance.forward[1] = cameraForward[1]; instance.forward[2] = cameraForward[2];
                // Thin needle stretched along the fall direction.
                instance.right[0] = streakRight[0] * widthScale;
                instance.right[1] = streakRight[1] * widthScale;
                instance.right[2] = streakRight[2] * widthScale;
                instance.up[0] = fallDir[0] * lengthScale;
                instance.up[1] = fallDir[1] * lengthScale;
                instance.up[2] = fallDir[2] * lengthScale;
                items.push_back(instance);
            }
        }
        else
        {
            for (std::uint32_t i = 0; i < snowActiveCount_; ++i)
            {
                auto& particle = snowParticles_[i];
                particle.offset[1] -= kSnowFallSpeed * dt;
                // Gentle horizontal drift so snow doesn't fall in dead-straight
                // lines — cheap per-particle sine sway, no extra state beyond
                // the particle's own (stable) seed-derived phase.
                const float phase = particle.offset[1] * 0.37f + static_cast<float>(i) * 12.9f;
                const float sway = std::sin(phase * kSnowSwaySpeed) * kSnowSwayAmplitude * dt;
                particle.offset[0] += sway;
                if (particle.offset[1] < kDespawnBottom)
                    respawn(particle, static_cast<float>(seedCounter_++) * 53.7f);

                const float size = kSnowSize * particle.sizeScale;
                EffectParticleInstance instance{};
                instance.color[0] = kSnowColor[0]; instance.color[1] = kSnowColor[1]; instance.color[2] = kSnowColor[2];
                instance.color[3] = kSnowAlpha;
                instance.position[0] = cameraPosition[0] + particle.offset[0];
                instance.position[1] = cameraPosition[1] + particle.offset[1];
                instance.position[2] = cameraPosition[2] + particle.offset[2];
                instance.forward[0] = cameraForward[0]; instance.forward[1] = cameraForward[1]; instance.forward[2] = cameraForward[2];
                instance.right[0] = cameraRight[0] * size; instance.right[1] = cameraRight[1] * size; instance.right[2] = cameraRight[2] * size;
                instance.up[0] = cameraUp[0] * size; instance.up[1] = cameraUp[1] * size; instance.up[2] = cameraUp[2] * size;
                items.push_back(instance);
            }
        }

        if (items.empty())
            return;

        const float corners[4][2]{ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } };
        const float uvs[4][2]{ { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f } };
        const auto layer = isRain ? kSoftStreakLayer : kSoftCircleLayer;

        for (int i = 0; i < 4; ++i)
        {
            TerrainVertex vertex{};
            vertex.position[0] = corners[i][0];
            vertex.position[1] = corners[i][1];
            vertex.position[2] = 0.0f;
            vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
            vertex.normal[0] = 0.0f; vertex.normal[1] = 0.0f; vertex.normal[2] = 1.0f;
            vertex.uv[0] = uvs[i][0]; vertex.uv[1] = uvs[i][1];
            vertex.textureLayer = layer;
            vertices_.push_back(vertex);
        }
        const std::uint32_t quadIndices[6]{ 0, 1, 2, 2, 1, 3 };
        for (const auto quadIndex : quadIndices)
            indices_.push_back(quadIndex);

        instances_ = std::move(items);

        EffectParticleBatch batch{};
        batch.firstIndex = 0;
        batch.indexCount = 6;
        batch.firstInstance = 0;
        batch.instanceCount = static_cast<std::uint32_t>(instances_.size());
        // Alpha blend (src=SRC_ALPHA, dst=ONE_MINUS_SRC_ALPHA in the
        // D3DBLEND-style table d3d_blend_to_gl reads).
        batch.sourceBlend = 4;
        batch.destinationBlend = 5;
        batches_.push_back(batch);
    }
}
