#pragma once

#include "renderer/opengl_renderer.h"

#include <cstdint>
#include <vector>

namespace phoenix::runtime
{
    enum class CelestialBody
    {
        None,
        Sun,
        Moon,
    };

    // Sun/moon as real world-space billboards, replacing sky.frag's previous
    // screen-space ray-reconstruction (which computed a world direction from
    // camera yaw/pitch independently of how every other object is
    // projected — any camera rotation moved the disc inconsistently, making
    // it read as tracking the camera instead of sitting still in the sky).
    //
    // Each frame this places one additive billboard at
    // cameraPosition + fixedWorldDirection * distance, where distance is
    // derived from the active far plane so it's always drawn safely inside
    // the view frustum. Recomputing from the CURRENT camera position every
    // frame (same technique WeatherParticleSystem uses for rain/snow) makes
    // it parallax-free under translation; going through the exact same
    // effect_particle.vert transform as every other rendered object makes it
    // rotate exactly like a real, correctly-distant object under yaw/pitch —
    // no separate math to get wrong. This positioning approach is the part
    // that's been validated and is meant to stay fixed; the visual look
    // itself (see effect_particle.frag's sun/moon sentinels) is its own
    // concern and can keep evolving independently.
    class CelestialSystem
    {
    public:
        // strength: 0 = invisible, 1 = full brightness. tint: RGB multiplier
        // applied on top of the shader's own sun/moon coloring — lets the
        // caller warm it for dawn/dusk or dim it for overcast without the
        // shader needing to know about weather.
        void update(CelestialBody body, float strength, const float tint[3],
            const float cameraPosition[3],
            const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
            float farPlane);

        bool has_particles() const { return !batches_.empty(); }
        const std::vector<phoenix::renderer::TerrainVertex>& vertices() const { return vertices_; }
        const std::vector<std::uint32_t>& indices() const { return indices_; }
        const std::vector<phoenix::renderer::EffectParticleInstance>& instances() const { return instances_; }
        const std::vector<phoenix::renderer::EffectParticleBatch>& batches() const { return batches_; }

    private:
        std::vector<phoenix::renderer::TerrainVertex> vertices_;
        std::vector<std::uint32_t> indices_;
        std::vector<phoenix::renderer::EffectParticleInstance> instances_;
        std::vector<phoenix::renderer::EffectParticleBatch> batches_;
    };
}
