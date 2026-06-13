#include "character/weapon_effect.h"

#include <algorithm>
#include <cmath>

namespace phoenix::character
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr std::size_t kMaxParticlesPerLayer = 1024;
    }

    WeaponEffect::WeaponEffect()
    {
        set_element(Element::Fire);
    }

    const char* WeaponEffect::element_name(Element e)
    {
        switch (e)
        {
        case Element::Fire:  return "Fire";
        case Element::Wind:  return "Wind";
        case Element::Earth: return "Earth";
        case Element::Water: return "Water";
        }
        return "?";
    }

    void WeaponEffect::set_element(Element e)
    {
        element_ = e;

        auto set = [](float (&c)[3], float r, float g, float b) { c[0] = r; c[1] = g; c[2] = b; };
        Layer& core   = layers_[0];  // main flow along the blade
        Layer& detail = layers_[1];  // small fast accents (sparks/wisps/drops)
        Layer& glow   = layers_[2];  // big soft halo near the guard
        core = {}; detail = {}; glow = {};

        switch (e)
        {
        case Element::Fire:
            // Bright tongue of flame licking up the blade, ember sparks, warm halo.
            set(core.colorStart, 1.00f, 0.85f, 0.35f); set(core.colorEnd, 0.95f, 0.22f, 0.02f);
            core.spawnRate = 120.0f; core.flowSpeed = 1.1f; core.lifetime = 0.55f;
            core.size = 0.045f; core.sizeGrowth = 0.55f; core.flicker = 0.35f;
            core.bladeLength = 0.85f; core.radius = 0.045f; core.swirl = 1.2f;

            set(detail.colorStart, 1.00f, 0.60f, 0.10f); set(detail.colorEnd, 0.55f, 0.05f, 0.00f);
            detail.spawnRate = 36.0f; detail.flowSpeed = 1.6f; detail.lifetime = 0.9f;
            detail.size = 0.018f; detail.sizeGrowth = 0.4f; detail.flicker = 0.6f;
            detail.bladeLength = 0.9f; detail.radius = 0.10f; detail.swirl = 3.5f;

            set(glow.colorStart, 0.90f, 0.30f, 0.05f); set(glow.colorEnd, 0.30f, 0.04f, 0.00f);
            glow.intensity = 0.5f; glow.spawnRate = 26.0f; glow.flowSpeed = 0.3f;
            glow.lifetime = 1.0f; glow.size = 0.085f; glow.sizeGrowth = 1.4f;
            glow.bladeLength = 0.5f; glow.radius = 0.02f; glow.swirl = 0.6f;
            break;

        case Element::Wind:
            // Two dense counter-rotating streams of pale wisps plus fast streaks.
            set(core.colorStart, 0.75f, 0.90f, 0.95f); set(core.colorEnd, 0.40f, 0.55f, 0.65f);
            core.intensity = 0.70f; core.spawnRate = 130.0f; core.flowSpeed = 1.4f;
            core.lifetime = 0.8f; core.size = 0.030f; core.sizeGrowth = 1.2f; core.flicker = 0.3f;
            core.bladeLength = 1.0f; core.radius = 0.10f; core.swirl = 6.0f;

            set(detail.colorStart, 0.80f, 0.95f, 0.85f); set(detail.colorEnd, 0.45f, 0.60f, 0.60f);
            detail.intensity = 0.60f; detail.spawnRate = 90.0f; detail.flowSpeed = 1.0f;
            detail.lifetime = 0.7f; detail.size = 0.024f; detail.sizeGrowth = 1.1f;
            detail.bladeLength = 1.0f; detail.radius = 0.13f; detail.swirl = -5.0f;

            set(glow.colorStart, 0.85f, 0.95f, 1.00f); set(glow.colorEnd, 0.45f, 0.55f, 0.65f);
            glow.intensity = 0.45f; glow.spawnRate = 45.0f; glow.flowSpeed = 1.8f;
            glow.lifetime = 0.45f; glow.size = 0.050f; glow.sizeGrowth = 0.6f; glow.flicker = 0.4f;
            glow.bladeLength = 0.95f; glow.radius = 0.06f; glow.swirl = 8.0f;
            break;

        case Element::Earth:
            // Green nature aura: leafy motes sinking slowly, bright moss sparks,
            // deep green dust core (just a hint of earthy warmth at birth).
            set(core.colorStart, 0.55f, 0.72f, 0.28f); set(core.colorEnd, 0.18f, 0.36f, 0.10f);
            core.spawnRate = 60.0f; core.flowSpeed = -0.35f; core.lifetime = 1.3f;
            core.size = 0.035f; core.sizeGrowth = 0.7f;
            core.bladeLength = 0.9f; core.radius = 0.12f; core.swirl = 0.8f;

            set(detail.colorStart, 0.50f, 0.90f, 0.30f); set(detail.colorEnd, 0.12f, 0.40f, 0.08f);
            detail.spawnRate = 28.0f; detail.flowSpeed = 0.25f; detail.lifetime = 1.0f;
            detail.size = 0.020f; detail.flicker = 0.5f;
            detail.bladeLength = 0.85f; detail.radius = 0.16f; detail.swirl = -1.4f;

            set(glow.colorStart, 0.30f, 0.50f, 0.18f); set(glow.colorEnd, 0.08f, 0.22f, 0.06f);
            glow.intensity = 0.45f; glow.spawnRate = 30.0f; glow.flowSpeed = 0.1f;
            glow.lifetime = 1.2f; glow.size = 0.070f; glow.sizeGrowth = 1.5f;
            glow.bladeLength = 0.55f; glow.radius = 0.04f; glow.swirl = 0.4f;
            break;

        case Element::Water:
            // Flowing aqua stream up the blade, bright droplets dripping down, mist.
            set(core.colorStart, 0.35f, 0.70f, 1.00f); set(core.colorEnd, 0.10f, 0.30f, 0.80f);
            core.spawnRate = 100.0f; core.flowSpeed = 0.8f; core.lifetime = 0.8f;
            core.size = 0.040f; core.sizeGrowth = 0.8f;
            core.bladeLength = 0.9f; core.radius = 0.05f; core.swirl = 2.2f;

            set(detail.colorStart, 0.70f, 0.92f, 1.00f); set(detail.colorEnd, 0.25f, 0.50f, 0.90f);
            detail.spawnRate = 40.0f; detail.flowSpeed = -0.5f; detail.lifetime = 1.0f;
            detail.size = 0.018f; detail.flicker = 0.4f;
            detail.bladeLength = 0.9f; detail.radius = 0.11f; detail.swirl = 1.2f;

            set(glow.colorStart, 0.30f, 0.55f, 0.85f); set(glow.colorEnd, 0.08f, 0.20f, 0.50f);
            glow.intensity = 0.40f; glow.spawnRate = 24.0f; glow.flowSpeed = 0.3f;
            glow.lifetime = 1.1f; glow.size = 0.075f; glow.sizeGrowth = 1.4f;
            glow.bladeLength = 0.6f; glow.radius = 0.03f; glow.swirl = 0.8f;
            break;
        }
    }

    void WeaponEffect::spawn(const Layer& layer, Particle& p)
    {
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        p.along = unit(rng_) * layer.bladeLength;
        p.angle = unit(rng_) * 2.0f * kPi;
        p.seedRadius = layer.radius * std::sqrt(unit(rng_));
        p.ageS = 0.0f;
        p.lifeS = layer.lifetime > 0.01f ? layer.lifetime : 0.9f;
        p.alive = true;
    }

    void WeaponEffect::simulate_layer(float dt, const Layer& layer, LayerRuntime& rt,
                                      const CharacterSystem::WeaponAttachment& attach,
                                      const CharacterSystem::WeaponAttachment* offhand,
                                      renderer::ParticleBatch& batch)
    {
        // ---- Spawn new particles at the configured rate (reusing dead slots). ----
        rt.spawnAccumulator += layer.spawnRate * dt;
        const auto maxAlive = std::min<std::size_t>(
            kMaxParticlesPerLayer,
            static_cast<std::size_t>(layer.spawnRate * layer.lifetime) + 32);
        while (rt.spawnAccumulator >= 1.0f)
        {
            rt.spawnAccumulator -= 1.0f;
            Particle* slot = nullptr;
            for (auto& p : rt.particles)
                if (!p.alive) { slot = &p; break; }
            if (!slot)
            {
                if (rt.particles.size() >= maxAlive)
                    break;
                rt.particles.emplace_back();
                slot = &rt.particles.back();
            }
            spawn(layer, *slot);
        }

        // ---- Weapon-local bases (columns include the character scale). ----
        // Dual-wield emits every particle on both weapons; the off-hand copy
        // gets its swirl phase shifted by pi so the auras don't look cloned.
        const CharacterSystem::WeaponAttachment* attaches[2] = { &attach, offhand };
        const int attachCount = (offhand && offhand->valid) ? 2 : 1;
        const float* cols[2][3];
        float scales[2];
        for (int ai = 0; ai < attachCount; ++ai)
        {
            const float* B = attaches[ai]->basis;
            cols[ai][0] = &B[0]; cols[ai][1] = &B[3]; cols[ai][2] = &B[6];
            scales[ai] = std::sqrt(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);
        }

        // Actual weapon extent along the blade (model space): particles live in
        // normalized [0,1] of this span so the aura hugs the real mesh bounds.
        const float bladeStart = attach.bladeStart;
        const float bladeSpan = std::max(0.15f, attach.bladeEnd - attach.bladeStart);

        const float intensity = std::clamp(layer.intensity, 0.0f, 4.0f);

        for (auto& p : rt.particles)
        {
            if (!p.alive)
                continue;
            p.ageS += dt;
            if (p.ageS >= p.lifeS)
            {
                p.alive = false;
                continue;
            }
            // Flow in model units/s, normalized to the blade span; particles
            // leaving the weapon bounds die instead of drifting past the tip.
            p.along += layer.flowSpeed * dt / bladeSpan;
            if (p.along < 0.0f || p.along > 1.0f)
            {
                p.alive = false;
                continue;
            }
            p.angle += layer.swirl * dt;

            const float t = p.ageS / p.lifeS;
            const float fade = (t < 0.2f) ? (t / 0.2f) : (1.0f - (t - 0.2f) / 0.8f);
            float alpha = std::clamp(fade, 0.0f, 1.0f) * intensity;
            // Soft taper at the guard and the tip so the aura visually ends
            // with the weapon instead of popping at the bounds.
            const float edge = std::min(p.along, 1.0f - p.along);
            alpha *= std::clamp(edge / 0.12f, 0.0f, 1.0f);
            // Cheap shimmer: phase from the particle's own angle/age, no RNG.
            if (layer.flicker > 0.0f)
                alpha *= 1.0f - layer.flicker * (0.5f + 0.5f * std::sin(p.angle * 9.0f + p.ageS * 30.0f));

            // Birth -> death colour gradient.
            const float r = layer.colorStart[0] + (layer.colorEnd[0] - layer.colorStart[0]) * t;
            const float g = layer.colorStart[1] + (layer.colorEnd[1] - layer.colorStart[1]) * t;
            const float b = layer.colorStart[2] + (layer.colorEnd[2] - layer.colorStart[2]) * t;

            for (int ai = 0; ai < attachCount; ++ai)
            {
                // Blade axis is Y in weapon-model space (matches retail meshes).
                const float angle = p.angle + (ai == 1 ? kPi : 0.0f);
                float local[3];
                local[1] = bladeStart + p.along * bladeSpan;
                local[2] = p.seedRadius * std::cos(angle);
                local[0] = p.seedRadius * std::sin(angle);

                const auto& at = *attaches[ai];
                const float* const* col = cols[ai];
                renderer::ParticleInstance inst;
                inst.position[0] = at.position[0] + col[0][0] * local[0] + col[1][0] * local[1] + col[2][0] * local[2];
                inst.position[1] = at.position[1] + col[0][1] * local[0] + col[1][1] * local[1] + col[2][1] * local[2];
                inst.position[2] = at.position[2] + col[0][2] * local[0] + col[1][2] * local[1] + col[2][2] * local[2];
                inst.size = layer.size * scales[ai] * (1.0f + (layer.sizeGrowth - 1.0f) * t);
                inst.color[0] = r;
                inst.color[1] = g;
                inst.color[2] = b;
                inst.color[3] = alpha;
                batch.add(inst, /*additiveBlend=*/true);   // weapon aura glows (additive)
            }
        }
    }

    void WeaponEffect::update(float dt,
                              const CharacterSystem::WeaponAttachment& attach,
                              renderer::ParticleBatch& batch,
                              const CharacterSystem::WeaponAttachment* offhand)
    {
        if (!enabled_ || !attach.valid)
        {
            // Stop emitting and reset pools so re-enabling starts clean.
            for (auto& rt : runtime_)
            {
                rt.particles.clear();
                rt.spawnAccumulator = 0.0f;
            }
            return;
        }

        dt = std::clamp(dt, 0.0f, 0.1f);

        for (int i = 0; i < kMaxLayers; ++i)
            simulate_layer(dt, layers_[static_cast<std::size_t>(i)],
                           runtime_[static_cast<std::size_t>(i)], attach, offhand, batch);
    }
}
