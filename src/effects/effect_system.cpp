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
        case EmitterShape::Shockwave:
        {
            const float c = std::cos(angle), s = std::sin(angle);
            local[0] = layer.radius * c;
            local[2] = layer.radius * s;
            dir[0] = c; dir[1] = 0.0f; dir[2] = s;   // radially outward, flat
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

    EffectDefinition preset_frost_nova()
    {
        EffectDefinition d;
        d.name = "Frost nova";
        d.loop = false; d.duration = 0.08f; d.layerCount = 2;
        auto& shard = d.layers[0];
        shard.shape = EmitterShape::Sphere; shard.blend = Blend::Additive;
        set3(shard.colorStart, 0.7f, 0.95f, 1.0f); set3(shard.colorEnd, 0.5f, 0.7f, 1.0f);
        shard.intensity = 1.1f; shard.spawnRate = 1600.0f; shard.lifetime = 0.5f;
        shard.size = 0.09f; shard.radius = 0.2f; shard.speed = 5.0f; shard.gravity = 1.5f; shard.drag = 2.5f;
        auto& mist = d.layers[1];
        mist.shape = EmitterShape::Disc; mist.blend = Blend::Alpha;
        set3(mist.colorStart, 0.8f, 0.92f, 1.0f); set3(mist.colorEnd, 0.6f, 0.75f, 0.95f);
        mist.intensity = 0.4f; mist.spawnRate = 200.0f; mist.lifetime = 0.7f;
        mist.size = 0.3f; mist.radius = 0.6f; mist.speed = 2.0f; mist.drag = 3.0f;
        return d;
    }

    EffectDefinition preset_shockwave()
    {
        EffectDefinition d;
        d.name = "Shockwave";
        d.loop = false; d.duration = 0.05f; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Shockwave; l.blend = Blend::Additive;
        set3(l.colorStart, 1.0f, 0.95f, 0.8f); set3(l.colorEnd, 1.0f, 0.6f, 0.2f);
        l.intensity = 1.2f; l.spawnRate = 2000.0f; l.lifetime = 0.4f;
        l.size = 0.12f; l.radius = 0.3f; l.speed = 10.0f; l.drag = 3.0f;
        return d;
    }

    EffectDefinition preset_blood_splatter()
    {
        EffectDefinition d;
        d.name = "Blood splatter";
        d.loop = false; d.duration = 0.05f; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Sphere; l.blend = Blend::Alpha;
        set3(l.colorStart, 0.7f, 0.05f, 0.05f); set3(l.colorEnd, 0.3f, 0.0f, 0.0f);
        l.intensity = 1.0f; l.spawnRate = 900.0f; l.lifetime = 0.6f;
        l.size = 0.08f; l.radius = 0.15f; l.speed = 4.5f; l.gravity = 12.0f; l.drag = 1.0f;
        return d;
    }

    EffectDefinition preset_lightning()
    {
        EffectDefinition d;
        d.name = "Lightning sparks";
        d.loop = false; d.duration = 0.12f; d.layerCount = 2;
        auto& arc = d.layers[0];
        arc.shape = EmitterShape::Point; arc.blend = Blend::Additive;
        set3(arc.colorStart, 0.8f, 0.9f, 1.0f); set3(arc.colorEnd, 0.4f, 0.5f, 1.0f);
        arc.intensity = 1.5f; arc.spawnRate = 1200.0f; arc.lifetime = 0.2f;
        arc.size = 0.07f; arc.speed = 9.0f; arc.gravity = 3.0f; arc.drag = 1.5f;
        auto& flash = d.layers[1];
        flash.shape = EmitterShape::Point; flash.blend = Blend::Additive;
        set3(flash.colorStart, 0.95f, 0.97f, 1.0f); set3(flash.colorEnd, 0.6f, 0.7f, 1.0f);
        flash.intensity = 1.6f; flash.spawnRate = 200.0f; flash.lifetime = 0.1f;
        flash.size = 0.35f; flash.speed = 0.5f;
        return d;
    }

    EffectDefinition preset_water_fountain()
    {
        EffectDefinition d;
        d.name = "Water fountain";
        d.loop = true; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Cone; l.blend = Blend::Alpha;
        set3(l.colorStart, 0.6f, 0.8f, 1.0f); set3(l.colorEnd, 0.85f, 0.95f, 1.0f);
        l.intensity = 0.7f; l.spawnRate = 300.0f; l.lifetime = 1.4f;
        l.size = 0.10f; l.speed = 6.0f; l.gravity = 9.0f; l.coneAngleDeg = 14.0f;
        return d;
    }

    EffectDefinition preset_smoke_plume()
    {
        EffectDefinition d;
        d.name = "Smoke plume";
        d.loop = true; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Disc; l.blend = Blend::Alpha;
        set3(l.colorStart, 0.30f, 0.30f, 0.32f); set3(l.colorEnd, 0.06f, 0.06f, 0.07f);
        l.intensity = 0.55f; l.spawnRate = 55.0f; l.lifetime = 2.4f;
        l.size = 0.45f; l.radius = 0.3f; l.speed = 1.4f; l.gravity = -0.7f; l.drag = 0.5f;
        return d;
    }

    EffectDefinition preset_embers()
    {
        EffectDefinition d;
        d.name = "Embers";
        d.loop = true; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Disc; l.blend = Blend::Additive;
        set3(l.colorStart, 1.0f, 0.6f, 0.15f); set3(l.colorEnd, 0.6f, 0.1f, 0.0f);
        l.intensity = 1.0f; l.spawnRate = 40.0f; l.lifetime = 2.0f;
        l.size = 0.05f; l.radius = 0.6f; l.speed = 1.0f; l.gravity = -0.6f; l.drag = 0.4f;
        return d;
    }

    EffectDefinition preset_fireflies()
    {
        EffectDefinition d;
        d.name = "Fireflies";
        d.loop = true; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Sphere; l.blend = Blend::Additive;
        set3(l.colorStart, 0.9f, 1.0f, 0.5f); set3(l.colorEnd, 0.4f, 0.8f, 0.2f);
        l.intensity = 0.9f; l.spawnRate = 25.0f; l.lifetime = 3.0f;
        l.size = 0.05f; l.radius = 2.0f; l.speed = 0.25f; l.gravity = -0.05f; l.drag = 0.3f;
        return d;
    }

    EffectDefinition preset_arcane_sigil()
    {
        EffectDefinition d;
        d.name = "Arcane sigil";
        d.loop = true; d.layerCount = 2;
        auto& ring = d.layers[0];
        ring.shape = EmitterShape::Ring; ring.blend = Blend::Additive;
        set3(ring.colorStart, 0.8f, 0.4f, 1.0f); set3(ring.colorEnd, 0.4f, 0.1f, 0.8f);
        ring.intensity = 1.0f; ring.spawnRate = 300.0f; ring.lifetime = 0.9f;
        ring.size = 0.08f; ring.radius = 1.6f; ring.speed = 0.3f; ring.gravity = -0.1f;
        auto& inner = d.layers[1];
        inner.shape = EmitterShape::Ring; inner.blend = Blend::Additive;
        set3(inner.colorStart, 0.95f, 0.7f, 1.0f); set3(inner.colorEnd, 0.5f, 0.2f, 0.9f);
        inner.intensity = 0.9f; inner.spawnRate = 200.0f; inner.lifetime = 0.9f;
        inner.size = 0.07f; inner.radius = 0.9f; inner.speed = 0.3f; inner.gravity = -0.1f;
        return d;
    }

    EffectDefinition preset_toxic_geyser()
    {
        EffectDefinition d;
        d.name = "Toxic geyser";
        d.loop = true; d.layerCount = 1;
        auto& l = d.layers[0];
        l.shape = EmitterShape::Cone; l.blend = Blend::Alpha;
        set3(l.colorStart, 0.5f, 1.0f, 0.25f); set3(l.colorEnd, 0.1f, 0.35f, 0.05f);
        l.intensity = 0.7f; l.spawnRate = 200.0f; l.lifetime = 1.6f;
        l.size = 0.22f; l.speed = 5.0f; l.gravity = 6.0f; l.coneAngleDeg = 22.0f; l.drag = 0.4f;
        return d;
    }

    EffectDefinition preset_shadow_flames()
    {
        EffectDefinition d;
        d.name = "Shadow flames";
        d.loop = true; d.layerCount = 2;
        auto& fire = d.layers[0];
        fire.shape = EmitterShape::Disc; fire.blend = Blend::Additive;
        set3(fire.colorStart, 0.5f, 0.15f, 0.7f); set3(fire.colorEnd, 0.15f, 0.0f, 0.25f);
        fire.intensity = 1.0f; fire.spawnRate = 240.0f; fire.lifetime = 0.9f;
        fire.size = 0.18f; fire.radius = 0.45f; fire.speed = 2.2f; fire.gravity = -1.4f; fire.drag = 1.1f;
        auto& wisp = d.layers[1];
        wisp.shape = EmitterShape::Disc; wisp.blend = Blend::Alpha;
        set3(wisp.colorStart, 0.1f, 0.0f, 0.15f); set3(wisp.colorEnd, 0.0f, 0.0f, 0.0f);
        wisp.intensity = 0.5f; wisp.spawnRate = 40.0f; wisp.lifetime = 1.5f;
        wisp.size = 0.3f; wisp.radius = 0.35f; wisp.speed = 1.6f; wisp.gravity = -0.8f; wisp.drag = 0.6f;
        return d;
    }

    EffectDefinition preset_rainbow_fountain()
    {
        // Three offset-coloured cones for a playful multi-colour spray.
        EffectDefinition d;
        d.name = "Rainbow fountain";
        d.loop = true; d.layerCount = 3;
        auto cone = [](EffectLayer& l, float r, float g, float b) {
            l.shape = EmitterShape::Cone; l.blend = Blend::Additive;
            l.colorStart[0] = r; l.colorStart[1] = g; l.colorStart[2] = b;
            l.colorEnd[0] = r * 0.4f; l.colorEnd[1] = g * 0.4f; l.colorEnd[2] = b * 0.4f;
            l.intensity = 1.0f; l.spawnRate = 130.0f; l.lifetime = 1.3f;
            l.size = 0.10f; l.speed = 5.5f; l.gravity = 8.0f; l.coneAngleDeg = 30.0f;
        };
        cone(d.layers[0], 1.0f, 0.2f, 0.2f);
        cone(d.layers[1], 0.2f, 1.0f, 0.3f);
        cone(d.layers[2], 0.3f, 0.4f, 1.0f);
        return d;
    }

    const std::vector<PresetEntry>& preset_catalog()
    {
        static const std::vector<PresetEntry> catalog = {
            { "Portal",            preset_portal },
            { "Fire pillar",       preset_fire_pillar },
            { "Holy column",       preset_holy_column },
            { "Poison cloud",      preset_poison_cloud },
            { "Arcane sigil",      preset_arcane_sigil },
            { "Shadow flames",     preset_shadow_flames },
            { "Water fountain",    preset_water_fountain },
            { "Rainbow fountain",  preset_rainbow_fountain },
            { "Toxic geyser",      preset_toxic_geyser },
            { "Smoke plume",       preset_smoke_plume },
            { "Embers",            preset_embers },
            { "Fireflies",         preset_fireflies },
            { "Impact (1-shot)",   preset_impact },
            { "Heal burst (1-shot)", preset_heal_burst },
            { "Frost nova (1-shot)", preset_frost_nova },
            { "Shockwave (1-shot)",  preset_shockwave },
            { "Blood splatter (1-shot)", preset_blood_splatter },
            { "Lightning (1-shot)",  preset_lightning },
        };
        return catalog;
    }
}
