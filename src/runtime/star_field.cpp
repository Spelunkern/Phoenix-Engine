#include "runtime/star_field.h"

#include <algorithm>
#include <cmath>

namespace phoenix::runtime
{
    namespace
    {
        using phoenix::renderer::TerrainVertex;
        using phoenix::renderer::EffectParticleInstance;
        using phoenix::renderer::EffectParticleBatch;

        // effect_particle.frag's soft-circle sentinel (also used by
        // WeatherParticleSystem's snow) — a small round dot, no texture.
        constexpr std::uint32_t kSoftCircleLayer = 0xFFFFFFFEu;

        constexpr int kStarCount = 3200;
        // Kept in sync with celestial_system.cpp's kMoonDir — stars within
        // this angular cone of the moon's fixed direction are skipped so
        // none render on top of it.
        constexpr float kMoonDir[3]{ 0.48f, 0.36f, -0.55f };
        constexpr float kMoonExclusionCos = 0.975f; // ~13 degrees

        float fract(float v) { return v - std::floor(v); }
        float random01(float seed) { return fract(std::sin(seed * 12.9898f) * 43758.5453f); }

        void normalize3(float v[3])
        {
            const float lenSq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            if (lenSq < 0.000001f)
                return;
            const float inv = 1.0f / std::sqrt(lenSq);
            v[0] *= inv; v[1] *= inv; v[2] *= inv;
        }

        void push_billboard(std::vector<EffectParticleInstance>& items,
            const float position[3], const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
            float size, const float color[3], float alpha)
        {
            EffectParticleInstance instance{};
            instance.color[0] = color[0]; instance.color[1] = color[1]; instance.color[2] = color[2]; instance.color[3] = alpha;
            instance.position[0] = position[0]; instance.position[1] = position[1]; instance.position[2] = position[2];
            instance.forward[0] = cameraForward[0]; instance.forward[1] = cameraForward[1]; instance.forward[2] = cameraForward[2];
            instance.right[0] = cameraRight[0] * size; instance.right[1] = cameraRight[1] * size; instance.right[2] = cameraRight[2] * size;
            instance.up[0] = cameraUp[0] * size; instance.up[1] = cameraUp[1] * size; instance.up[2] = cameraUp[2] * size;
            items.push_back(instance);
        }
    }

    void StarField::build()
    {
        stars_.clear();
        stars_.reserve(kStarCount);

        float moonDir[3]{ kMoonDir[0], kMoonDir[1], kMoonDir[2] };
        normalize3(moonDir);

        std::uint32_t seedCounter = 1;
        int attempts = 0;
        while (static_cast<int>(stars_.size()) < kStarCount && attempts < kStarCount * 20)
        {
            ++attempts;
            const float seed = static_cast<float>(seedCounter++) * 17.23f;

            // Elevation biased toward the zenith (more stars overhead, fewer
            // near the horizon — a cheap stand-in for atmospheric
            // extinction) but never below a minimum so none sit at/under
            // the horizon line to begin with.
            const float minSinElev = 0.06f;
            const float elevSample = random01(seed + 1.0f);
            const float sinElev = minSinElev + (1.0f - minSinElev) * elevSample * elevSample;
            const float cosElev = std::sqrt(std::max(0.0f, 1.0f - sinElev * sinElev));
            const float azimuth = random01(seed + 2.0f) * 6.2831853f;

            float direction[3]{ cosElev * std::cos(azimuth), sinElev, cosElev * std::sin(azimuth) };

            const float dot = direction[0] * moonDir[0] + direction[1] * moonDir[1] + direction[2] * moonDir[2];
            if (dot > kMoonExclusionCos)
                continue;

            Star star{};
            star.direction[0] = direction[0]; star.direction[1] = direction[1]; star.direction[2] = direction[2];

            // Magnitude-like distribution: most stars small/dim, a handful
            // notably bigger/brighter.
            const float magnitude = random01(seed + 3.0f);
            const float weight = magnitude * magnitude * magnitude;
            star.size = 0.0018f + weight * 0.0032f;
            star.brightness = 0.35f + weight * 0.65f;

            // Mostly white, with an occasional warm or cool tinge — real
            // starlight varies subtly by stellar temperature.
            const float tintRoll = random01(seed + 4.0f);
            if (tintRoll < 0.15f)
            {
                star.color[0] = 1.0f; star.color[1] = 0.90f; star.color[2] = 0.78f; // warm
            }
            else if (tintRoll < 0.30f)
            {
                star.color[0] = 0.80f; star.color[1] = 0.88f; star.color[2] = 1.0f; // cool
            }
            else
            {
                star.color[0] = 1.0f; star.color[1] = 1.0f; star.color[2] = 1.0f;
            }

            // Only a small minority twinkle, and rarely — see update().
            star.twinkles = random01(seed + 5.0f) < 0.10f;
            star.twinklePeriod = 5.0f + random01(seed + 6.0f) * 7.0f;
            star.twinklePhase = random01(seed + 7.0f) * 100.0f;

            stars_.push_back(star);
        }
    }

    void StarField::update(bool visible,
        const float cameraPosition[3],
        const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
        float farPlane, float totalTime)
    {
        vertices_.clear();
        indices_.clear();
        instances_.clear();
        batches_.clear();

        if (!visible || stars_.empty())
            return;

        const float distance = std::max(80.0f, farPlane) * 0.92f;

        std::vector<EffectParticleInstance> items;
        items.reserve(stars_.size());
        for (const auto& star : stars_)
        {
            float brightness = star.brightness;
            if (star.twinkles)
            {
                const float cyclePos = fract(totalTime / star.twinklePeriod + star.twinklePhase);
                const float pulse = std::pow(std::max(0.0f, std::sin(cyclePos * 3.14159265f)), 48.0f);
                brightness *= 1.0f + pulse * 2.0f;
            }

            const float position[3]{
                cameraPosition[0] + star.direction[0] * distance,
                cameraPosition[1] + star.direction[1] * distance,
                cameraPosition[2] + star.direction[2] * distance,
            };
            push_billboard(items, position, cameraRight, cameraUp, cameraForward,
                distance * star.size, star.color, std::min(1.0f, brightness));
        }

        const float corners[4][2]{ { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } };
        const float uvs[4][2]{ { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f } };
        for (int i = 0; i < 4; ++i)
        {
            TerrainVertex vertex{};
            vertex.position[0] = corners[i][0];
            vertex.position[1] = corners[i][1];
            vertex.position[2] = 0.0f;
            vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
            vertex.normal[0] = 0.0f; vertex.normal[1] = 0.0f; vertex.normal[2] = 1.0f;
            vertex.uv[0] = uvs[i][0]; vertex.uv[1] = uvs[i][1];
            vertex.textureLayer = kSoftCircleLayer;
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
        // Standard alpha blend (src=SRC_ALPHA, dst=ONE_MINUS_SRC_ALPHA) —
        // same convention the soft-circle sentinel already uses for snow.
        batch.sourceBlend = 4;
        batch.destinationBlend = 5;
        batches_.push_back(batch);
    }
}
