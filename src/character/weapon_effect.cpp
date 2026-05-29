#include "character/weapon_effect.h"

#include <algorithm>
#include <cmath>

namespace phoenix::character
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr std::size_t kMaxParticlesPerLayer = 4096;
    }

    WeaponEffect::WeaponEffect()
    {
        // Layer 0 ships with a pleasant blue gradient so the feature looks good the
        // moment it is enabled; extra layers start empty/disabled.
        apply_preset(0, Preset::Arcane);
        layers_[0].enabled = true;
    }

    const char* WeaponEffect::preset_name(Preset p)
    {
        switch (p)
        {
        case Preset::Fire:   return "Fire";
        case Preset::Ice:    return "Ice";
        case Preset::Holy:   return "Holy";
        case Preset::Poison: return "Poison";
        case Preset::Shadow: return "Shadow";
        case Preset::Arcane: return "Arcane";
        }
        return "?";
    }

    void WeaponEffect::apply_preset(int layerIndex, Preset preset)
    {
        if (layerIndex < 0 || layerIndex >= kMaxLayers)
            return;
        Layer& l = layers_[static_cast<std::size_t>(layerIndex)];

        // Shared sensible defaults; presets override colour + a few knobs.
        l.intensity = 1.0f;
        l.spawnRate = 110.0f;
        l.flowSpeed = 0.7f;
        l.lifetime = 0.9f;
        l.size = 0.05f;
        l.bladeLength = 0.9f;
        l.radius = 0.06f;
        l.swirl = 2.0f;
        l.axis = 1;
        l.enabled = true;

        auto set = [](float (&c)[3], float r, float g, float b) { c[0] = r; c[1] = g; c[2] = b; };
        switch (preset)
        {
        case Preset::Fire:
            set(l.colorStart, 1.0f, 0.65f, 0.15f); set(l.colorEnd, 0.7f, 0.08f, 0.0f);
            l.flowSpeed = 1.0f; l.swirl = 1.0f; l.lifetime = 0.7f; break;
        case Preset::Ice:
            set(l.colorStart, 0.6f, 0.9f, 1.0f); set(l.colorEnd, 0.85f, 0.95f, 1.0f);
            l.flowSpeed = 0.4f; l.swirl = 2.5f; break;
        case Preset::Holy:
            set(l.colorStart, 1.0f, 0.92f, 0.55f); set(l.colorEnd, 1.0f, 1.0f, 0.95f);
            l.flowSpeed = 0.6f; l.swirl = 1.5f; break;
        case Preset::Poison:
            set(l.colorStart, 0.55f, 1.0f, 0.3f); set(l.colorEnd, 0.1f, 0.35f, 0.0f);
            l.flowSpeed = 0.5f; l.swirl = 3.0f; break;
        case Preset::Shadow:
            set(l.colorStart, 0.5f, 0.2f, 0.7f); set(l.colorEnd, 0.05f, 0.0f, 0.12f);
            l.flowSpeed = 0.5f; l.swirl = 2.0f; break;
        case Preset::Arcane:
            set(l.colorStart, 0.55f, 0.45f, 1.0f); set(l.colorEnd, 0.15f, 0.25f, 0.6f);
            l.flowSpeed = 0.7f; l.swirl = 2.0f; break;
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

        // ---- Weapon-local basis (columns include the character scale). ----
        const float* B = attach.basis;
        const float* col[3] = { &B[0], &B[3], &B[6] };
        const float scale = std::sqrt(B[0] * B[0] + B[1] * B[1] + B[2] * B[2]);

        const int a = std::clamp(layer.axis, 0, 2);
        const int p1 = (a + 1) % 3;
        const int p2 = (a + 2) % 3;
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
            p.along += layer.flowSpeed * dt;
            p.angle += layer.swirl * dt;

            float local[3]{};
            local[a]  = p.along;
            local[p1] = p.seedRadius * std::cos(p.angle);
            local[p2] = p.seedRadius * std::sin(p.angle);

            const float t = p.ageS / p.lifeS;
            const float fade = (t < 0.2f) ? (t / 0.2f) : (1.0f - (t - 0.2f) / 0.8f);
            const float alpha = std::clamp(fade, 0.0f, 1.0f) * intensity;

            // Birth -> death colour gradient.
            const float r = layer.colorStart[0] + (layer.colorEnd[0] - layer.colorStart[0]) * t;
            const float g = layer.colorStart[1] + (layer.colorEnd[1] - layer.colorStart[1]) * t;
            const float b = layer.colorStart[2] + (layer.colorEnd[2] - layer.colorStart[2]) * t;

            renderer::ParticleInstance inst;
            inst.position[0] = attach.position[0] + col[0][0] * local[0] + col[1][0] * local[1] + col[2][0] * local[2];
            inst.position[1] = attach.position[1] + col[0][1] * local[0] + col[1][1] * local[1] + col[2][1] * local[2];
            inst.position[2] = attach.position[2] + col[0][2] * local[0] + col[1][2] * local[1] + col[2][2] * local[2];
            inst.size = layer.size * scale;
            inst.color[0] = r;
            inst.color[1] = g;
            inst.color[2] = b;
            inst.color[3] = alpha;
            batch.add(inst, /*additiveBlend=*/true);   // weapon aura glows (additive)
        }
    }

    void WeaponEffect::update(float dt,
                              const CharacterSystem::WeaponAttachment& attach,
                              renderer::ParticleBatch& batch)
    {
        bool anyLayer = false;
        if (enabled_)
            for (const auto& l : layers_)
                anyLayer = anyLayer || l.enabled;

        if (!enabled_ || !anyLayer || !attach.valid)
        {
            // Let particles die out naturally is unnecessary here; just stop emitting
            // and reset pools so re-enabling starts clean.
            for (auto& rt : runtime_)
            {
                rt.particles.clear();
                rt.spawnAccumulator = 0.0f;
            }
            return;
        }

        dt = std::clamp(dt, 0.0f, 0.1f);

        for (int i = 0; i < kMaxLayers; ++i)
        {
            if (layers_[static_cast<std::size_t>(i)].enabled)
                simulate_layer(dt, layers_[static_cast<std::size_t>(i)],
                               runtime_[static_cast<std::size_t>(i)], attach, batch);
        }
    }
}
