#pragma once

#include "renderer/opengl_renderer.h"

#include <cstdint>
#include <vector>

namespace phoenix::runtime
{
    enum class WeatherIntensity
    {
        Light,
        Medium,
        Heavy,
    };

    // Custom GPU billboard rain/snow — fully procedural, no texture asset or
    // .seff data (weather.seff turned out to describe short ground-level
    // impact bursts, not a falling curtain; see git history for that
    // attempt). Reuses the same TerrainVertex/EffectParticleInstance/
    // EffectParticleBatch geometry OpenGLRenderer::update_effect_particles()
    // already draws for .EFT map effects — the caller merges this system's
    // buffers with EffectParticleSystem's before uploading. Rain/snow shapes
    // are drawn by effect_particle.frag's texture-less sentinels (soft
    // streak / soft circle), so there's no texture to resolve or fail to
    // resolve.
    class WeatherParticleSystem
    {
    public:
        // kind: 0 = none, 1 = rain, 2 = snow.
        void set_weather(int kind, WeatherIntensity intensity);

        void update(float deltaSeconds,
            const float cameraPosition[3],
            const float cameraRight[3], const float cameraUp[3], const float cameraForward[3]);

        bool has_particles() const { return !batches_.empty(); }
        const std::vector<phoenix::renderer::TerrainVertex>& vertices() const { return vertices_; }
        const std::vector<std::uint32_t>& indices() const { return indices_; }
        const std::vector<phoenix::renderer::EffectParticleInstance>& instances() const { return instances_; }
        const std::vector<phoenix::renderer::EffectParticleBatch>& batches() const { return batches_; }

    private:
        struct Particle
        {
            float offset[3]{}; // position relative to the camera
            float sizeScale{ 1.0f };
        };

        void respawn(Particle& particle, float seed) const;

        int kind_{};
        WeatherIntensity intensity_{ WeatherIntensity::Medium };
        std::uint32_t rainActiveCount_{};
        std::uint32_t snowActiveCount_{};
        std::vector<Particle> rainParticles_;
        std::vector<Particle> snowParticles_;
        std::uint32_t seedCounter_{};

        std::vector<phoenix::renderer::TerrainVertex> vertices_;
        std::vector<std::uint32_t> indices_;
        std::vector<phoenix::renderer::EffectParticleInstance> instances_;
        std::vector<phoenix::renderer::EffectParticleBatch> batches_;
    };
}
