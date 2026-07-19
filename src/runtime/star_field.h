#pragma once

#include "renderer/opengl_renderer.h"

#include <cstdint>
#include <vector>

namespace phoenix::runtime
{
    // A cheap, genuinely static night sky starfield — real world-space
    // billboards (same technique as CelestialSystem's sun/moon: fixed
    // per-star world direction, position recomputed each frame as
    // cameraPosition + direction * distance) instead of a per-pixel hash in
    // sky.frag. That screen-space approach looked plausible at a glance but
    // wasn't actually stable under camera rotation (the same root-cause bug
    // the sun/moon had before switching to real billboards) and had no depth
    // awareness, so it drew straight through water/terrain. Real billboards
    // fix both for free: they inherit the effect-particle pass's normal
    // depth test against opaque scene geometry, and reuse the exact same
    // proven camera transform as everything else.
    //
    // Directions are generated once in build() and never move; a small cone
    // around the moon's own fixed direction is excluded so stars don't
    // visibly collide with it (see celestial_system.cpp's kMoonDir — kept in
    // sync here). Twinkling is a rare, per-star pulse computed on the CPU
    // each frame — most stars hold a constant brightness.
    class StarField
    {
    public:
        void build();

        void update(bool visible,
            const float cameraPosition[3],
            const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
            float farPlane, float totalTime);

        bool has_particles() const { return !batches_.empty(); }
        const std::vector<phoenix::renderer::TerrainVertex>& vertices() const { return vertices_; }
        const std::vector<std::uint32_t>& indices() const { return indices_; }
        const std::vector<phoenix::renderer::EffectParticleInstance>& instances() const { return instances_; }
        const std::vector<phoenix::renderer::EffectParticleBatch>& batches() const { return batches_; }

    private:
        struct Star
        {
            float direction[3]{};
            float size{ 1.0f };
            float brightness{ 1.0f };
            float color[3]{ 1.0f, 1.0f, 1.0f };
            bool twinkles{};
            float twinklePeriod{ 6.0f };
            float twinklePhase{};
        };

        std::vector<Star> stars_;

        std::vector<phoenix::renderer::TerrainVertex> vertices_;
        std::vector<std::uint32_t> indices_;
        std::vector<phoenix::renderer::EffectParticleInstance> instances_;
        std::vector<phoenix::renderer::EffectParticleBatch> batches_;
    };
}
