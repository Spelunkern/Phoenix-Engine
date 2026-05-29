#include "effects/effect_system.h"

#include <algorithm>
#include <cmath>

namespace phoenix::effects
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;
        constexpr std::size_t kMaxParticlesPerLayer = 8192;

        // world = anchor.position + basis * local
        void local_to_world(const EffectAnchor& a, const float local[3], float out[3])
        {
            const float* B = a.basis;
            out[0] = a.position[0] + B[0] * local[0] + B[3] * local[1] + B[6] * local[2];
            out[1] = a.position[1] + B[1] * local[0] + B[4] * local[1] + B[7] * local[2];
            out[2] = a.position[2] + B[2] * local[0] + B[5] * local[1] + B[8] * local[2];
        }

        // Rotate a local direction/velocity into world space (basis linear part).
        void local_dir_to_world(const EffectAnchor& a, const float local[3], float out[3])
        {
            const float* B = a.basis;
            out[0] = B[0] * local[0] + B[3] * local[1] + B[6] * local[2];
            out[1] = B[1] * local[0] + B[4] * local[1] + B[7] * local[2];
            out[2] = B[2] * local[0] + B[5] * local[1] + B[8] * local[2];
        }
    }

    void EffectManager::spawn_particle(const EffectLayer& layer, const EffectAnchor& anchor, Particle& out)
    {
        std::uniform_real_distribution<float> sym(-1.0f, 1.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        float local[3]{ 0.0f, 0.0f, 0.0f };
        float dir[3]{ 0.0f, 1.0f, 0.0f };

        const float angle = unit(rng_) * 2.0f * kPi;

        switch (layer.shape)
        {
        case EmitterShape::Point:
        {
            float d[3]{ sym(rng_), sym(rng_), sym(rng_) };
            float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 1e-4f) { d[1] = 1.0f; len = 1.0f; }
            dir[0] = d[0] / len; dir[1] = d[1] / len; dir[2] = d[2] / len;
            break;
        }
        case EmitterShape::Sphere:
        {
            float d[3]{ sym(rng_), sym(rng_), sym(rng_) };
            float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (len < 1e-4f) { d[1] = 1.0f; len = 1.0f; }
            const float r = layer.radius * std::cbrt(unit(rng_));
            local[0] = d[0] / len * r; local[1] = d[1] / len * r; local[2] = d[2] / len * r;
            dir[0] = d[0] / len; dir[1] = d[1] / len; dir[2] = d[2] / len;  // outward
            break;
        }
        case EmitterShape::Ring:
        {
            local[0] = layer.radius * std::cos(angle);
            local[2] = layer.radius * std::sin(angle);
            dir[0] = 0.0f; dir[1] = 1.0f; dir[2] = 0.0f;  // rise
            break;
        }
        case EmitterShape::Disc:
        {
            const float r = layer.radius * std::sqrt(unit(rng_));
            local[0] = r * std::cos(angle);
            local[2] = r * std::sin(angle);
            dir[1] = 1.0f;
            break;
        }
        case EmitterShape::Cone:
        {
            const float half = std::clamp(layer.coneAngleDeg, 0.0f, 89.0f) * kPi / 180.0f;
            const float ca = std::cos(half);
            const float cosT = 1.0f - unit(rng_) * (1.0f - ca);  // within cone around +Y
            const float sinT = std::sqrt(std::max(0.0f, 1.0f - cosT * cosT));
            dir[0] = sinT * std::cos(angle);
            dir[1] = cosT;
            dir[2] = sinT * std::sin(angle);
            break;
        }
        case EmitterShape::Line:
        {
            local[1] = unit(rng_) * layer.height;
            dir[0] = sym(rng_) * 0.3f; dir[1] = 1.0f; dir[2] = sym(rng_) * 0.3f;
            break;
        }
        }

        float vel[3]{ dir[0] * layer.speed, dir[1] * layer.speed, dir[2] * layer.speed };

        local_to_world(anchor, local, out.pos);
        local_dir_to_world(anchor, vel, out.vel);
        out.ageS = 0.0f;
        out.lifeS = layer.lifetime > 0.01f ? layer.lifetime : 0.6f;
        out.alive = true;
    }

    EffectManager::Instance* EffectManager::find(Handle handle)
    {
        for (auto& inst : instances_)
            if (inst.id == handle)
                return &inst;
        return nullptr;
    }

    EffectManager::Handle EffectManager::spawn(const EffectDefinition& def, const EffectAnchor& anchor)
    {
        Instance inst;
        inst.id = nextHandle_++;
        inst.def = def;
        inst.anchor = anchor;
        inst.ageS = 0.0f;
        inst.emitting = true;
        instances_.push_back(std::move(inst));
        return instances_.back().id;
    }

    void EffectManager::set_anchor(Handle handle, const EffectAnchor& anchor)
    {
        if (auto* inst = find(handle))
            inst->anchor = anchor;
    }

    void EffectManager::stop(Handle handle)
    {
        if (auto* inst = find(handle))
            inst->emitting = false;
    }

    void EffectManager::clear()
    {
        instances_.clear();
    }

    void EffectManager::update(float dt, renderer::ParticleBatch& batch)
    {
        dt = std::clamp(dt, 0.0f, 0.1f);

        for (auto& inst : instances_)
        {
            inst.ageS += dt;
            // One-shots stop emitting after their duration; loops emit forever.
            if (!inst.def.loop && inst.ageS >= inst.def.duration)
                inst.emitting = false;

            const int layers = std::clamp(inst.def.layerCount, 0, kMaxEffectLayers);
            for (int li = 0; li < layers; ++li)
            {
                const auto& layer = inst.def.layers[static_cast<std::size_t>(li)];
                if (!layer.enabled)
                    continue;
                auto& rt = inst.runtime[static_cast<std::size_t>(li)];

                // Spawn (only while emitting), reusing dead slots.
                if (inst.emitting)
                {
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
                        spawn_particle(layer, inst.anchor, *slot);
                    }
                }

                // Integrate + emit.
                const float intensity = std::clamp(layer.intensity, 0.0f, 4.0f);
                const float dragFactor = std::clamp(1.0f - layer.drag * dt, 0.0f, 1.0f);
                const bool additive = (layer.blend == Blend::Additive);
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
                    p.vel[1] -= layer.gravity * dt;        // gravity>0 pulls down; <0 rises
                    p.vel[0] *= dragFactor; p.vel[1] *= dragFactor; p.vel[2] *= dragFactor;
                    p.pos[0] += p.vel[0] * dt;
                    p.pos[1] += p.vel[1] * dt;
                    p.pos[2] += p.vel[2] * dt;

                    const float t = p.ageS / p.lifeS;
                    const float fade = (t < 0.15f) ? (t / 0.15f) : (1.0f - (t - 0.15f) / 0.85f);

                    renderer::ParticleInstance bp;
                    bp.position[0] = p.pos[0];
                    bp.position[1] = p.pos[1];
                    bp.position[2] = p.pos[2];
                    bp.size = layer.size;
                    bp.color[0] = layer.colorStart[0] + (layer.colorEnd[0] - layer.colorStart[0]) * t;
                    bp.color[1] = layer.colorStart[1] + (layer.colorEnd[1] - layer.colorStart[1]) * t;
                    bp.color[2] = layer.colorStart[2] + (layer.colorEnd[2] - layer.colorStart[2]) * t;
                    bp.color[3] = std::clamp(fade, 0.0f, 1.0f) * intensity;
                    batch.add(bp, additive);
                }
            }
        }

        // Despawn finished one-shots once all their particles have died.
        instances_.erase(
            std::remove_if(instances_.begin(), instances_.end(), [](const Instance& inst) {
                if (inst.emitting)
                    return false;
                for (const auto& rt : inst.runtime)
                    for (const auto& p : rt.particles)
                        if (p.alive)
                            return false;
                return true;
            }),
            instances_.end());
    }

    // ---- Presets -------------------------------------------------------------
    namespace
    {
        void set3(float (&c)[3], float r, float g, float b) { c[0] = r; c[1] = g; c[2] = b; }
    }

    EffectDefinition preset_portal()
    {
        EffectDefinition d;
        d.name = "Portal";
        d.loop = true;
        d.layerCount = 2;
        // Rising ring rim.
        auto& rim = d.layers[0];
        rim.shape = EmitterShape::Ring;
        rim.blend = Blend::Additive;
        set3(rim.colorStart, 0.55f, 0.45f, 1.0f);
        set3(rim.colorEnd, 0.15f, 0.25f, 0.7f);
        rim.intensity = 1.0f; rim.spawnRate = 220.0f; rim.lifetime = 1.1f;
        rim.size = 0.10f; rim.radius = 1.4f; rim.speed = 0.8f; rim.gravity = -0.2f;
        // Inner swirl glow (disc).
        auto& core = d.layers[1];
        core.shape = EmitterShape::Disc;
        core.blend = Blend::Additive;
        set3(core.colorStart, 0.7f, 0.6f, 1.0f);
        set3(core.colorEnd, 0.2f, 0.1f, 0.6f);
        core.intensity = 0.8f; core.spawnRate = 90.0f; core.lifetime = 0.9f;
        core.size = 0.14f; core.radius = 1.1f; core.speed = 0.5f; core.gravity = -0.4f;
        return d;
    }

    EffectDefinition preset_fire_pillar()
    {
        EffectDefinition d;
        d.name = "Fire pillar";
        d.loop = true;
        d.layerCount = 2;
        auto& flame = d.layers[0];
        flame.shape = EmitterShape::Disc;
        flame.blend = Blend::Additive;
        set3(flame.colorStart, 1.0f, 0.7f, 0.2f);
        set3(flame.colorEnd, 0.7f, 0.05f, 0.0f);
        flame.intensity = 1.0f; flame.spawnRate = 260.0f; flame.lifetime = 0.8f;
        flame.size = 0.18f; flame.radius = 0.5f; flame.speed = 2.4f; flame.gravity = -1.5f; flame.drag = 1.2f;
        auto& smoke = d.layers[1];
        smoke.shape = EmitterShape::Disc;
        smoke.blend = Blend::Alpha;
        set3(smoke.colorStart, 0.25f, 0.22f, 0.2f);
        set3(smoke.colorEnd, 0.05f, 0.05f, 0.05f);
        smoke.intensity = 0.5f; smoke.spawnRate = 40.0f; smoke.lifetime = 1.6f;
        smoke.size = 0.35f; smoke.radius = 0.4f; smoke.speed = 1.6f; smoke.gravity = -0.8f; smoke.drag = 0.6f;
        return d;
    }

    EffectDefinition preset_holy_column()
    {
        EffectDefinition d;
        d.name = "Holy column";
        d.loop = true;
        d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Line;
        l.blend = Blend::Additive;
        set3(l.colorStart, 1.0f, 0.95f, 0.6f);
        set3(l.colorEnd, 1.0f, 1.0f, 0.95f);
        l.intensity = 1.0f; l.spawnRate = 160.0f; l.lifetime = 1.2f;
        l.size = 0.13f; l.height = 3.0f; l.speed = 1.2f; l.gravity = -0.5f;
        return d;
    }

    EffectDefinition preset_poison_cloud()
    {
        EffectDefinition d;
        d.name = "Poison cloud";
        d.loop = true;
        d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Disc;
        l.blend = Blend::Alpha;
        set3(l.colorStart, 0.5f, 0.9f, 0.25f);
        set3(l.colorEnd, 0.1f, 0.3f, 0.05f);
        l.intensity = 0.6f; l.spawnRate = 70.0f; l.lifetime = 2.2f;
        l.size = 0.4f; l.radius = 1.3f; l.speed = 0.4f; l.gravity = -0.15f; l.drag = 0.8f;
        return d;
    }

    EffectDefinition preset_impact()
    {
        EffectDefinition d;
        d.name = "Impact";
        d.loop = false;
        d.duration = 0.06f;
        d.layerCount = 2;
        auto& spark = d.layers[0];
        spark.shape = EmitterShape::Sphere;
        spark.blend = Blend::Additive;
        set3(spark.colorStart, 1.0f, 0.85f, 0.4f);
        set3(spark.colorEnd, 0.9f, 0.2f, 0.05f);
        spark.intensity = 1.2f; spark.spawnRate = 1400.0f; spark.lifetime = 0.35f;
        spark.size = 0.08f; spark.radius = 0.1f; spark.speed = 6.0f; spark.gravity = 8.0f; spark.drag = 2.0f;
        auto& flash = d.layers[1];
        flash.shape = EmitterShape::Point;
        flash.blend = Blend::Additive;
        set3(flash.colorStart, 1.0f, 0.95f, 0.7f);
        set3(flash.colorEnd, 1.0f, 0.5f, 0.1f);
        flash.intensity = 1.5f; flash.spawnRate = 300.0f; flash.lifetime = 0.12f;
        flash.size = 0.3f; flash.radius = 0.05f; flash.speed = 0.5f;
        return d;
    }

    EffectDefinition preset_heal_burst()
    {
        EffectDefinition d;
        d.name = "Heal burst";
        d.loop = false;
        d.duration = 0.5f;
        d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Disc;
        l.blend = Blend::Additive;
        set3(l.colorStart, 0.4f, 1.0f, 0.5f);
        set3(l.colorEnd, 0.9f, 1.0f, 0.8f);
        l.intensity = 1.1f; l.spawnRate = 240.0f; l.lifetime = 1.0f;
        l.size = 0.12f; l.radius = 0.7f; l.speed = 2.2f; l.gravity = -1.0f; l.drag = 1.0f;
        return d;
    }
}
