#include "effects/effect_system.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

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

        local[1] += layer.originHeight;   // lift the spawn (e.g. falling from the sky)

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

    EffectManager::Handle EffectManager::spawn(const EffectDefinition& def, const EffectAnchor& anchor,
                                               const float velocity[3], float travelTime)
    {
        Instance inst;
        inst.id = nextHandle_++;
        inst.def = def;
        inst.anchor = anchor;
        inst.ageS = 0.0f;
        inst.emitting = true;
        if (velocity && travelTime > 0.0f)
        {
            inst.moving = true;
            inst.velocity[0] = velocity[0];
            inst.velocity[1] = velocity[1];
            inst.velocity[2] = velocity[2];
            inst.travelTime = travelTime;
        }
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

            // Projectiles: advance the anchor; stop emitting once the flight ends
            // so the trailing particles fade out where it landed.
            if (inst.moving)
            {
                if (inst.ageS < inst.travelTime)
                {
                    inst.velocity[1] -= inst.def.projectileGravity * dt;  // arc
                    inst.anchor.position[0] += inst.velocity[0] * dt;
                    inst.anchor.position[1] += inst.velocity[1] * dt;
                    inst.anchor.position[2] += inst.velocity[2] * dt;
                }
                else
                {
                    inst.emitting = false;
                }
            }

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
                const float cx = inst.anchor.position[0];
                const float cz = inst.anchor.position[2];
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
                    // Vortex: drive horizontal velocity tangentially around the anchor.
                    if (layer.swirl != 0.0f)
                    {
                        const float dx = p.pos[0] - cx;
                        const float dz = p.pos[2] - cz;
                        const float r = std::sqrt(dx * dx + dz * dz);
                        if (r > 0.05f)
                        {
                            const float inv = layer.swirl / r;
                            p.vel[0] = -dz * inv;
                            p.vel[2] = dx * inv;
                        }
                    }
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


    // ---- Preset library ------------------------------------------------------
    namespace
    {
        // Compact layer builder. Less-common fields (drag/intensity/cone/height)
        // are trailing with sensible defaults so most calls stay short.
        // gravity > 0 pulls particles down; gravity < 0 makes them rise.
        EffectLayer mkLayer(EmitterShape shape, Blend blend,
            float sr, float sg, float sb,   // colour at birth
            float er, float eg, float eb,   // colour at death
            float spawnRate, float lifetime, float size,
            float speed, float gravity, float radius,
            float drag = 0.0f, float intensity = 1.0f,
            float coneAngleDeg = 25.0f, float height = 1.0f,
            float originHeight = 0.0f, float swirl = 0.0f)
        {
            EffectLayer l;
            l.shape = shape; l.blend = blend;
            l.colorStart[0] = sr; l.colorStart[1] = sg; l.colorStart[2] = sb;
            l.colorEnd[0] = er; l.colorEnd[1] = eg; l.colorEnd[2] = eb;
            l.spawnRate = spawnRate; l.lifetime = lifetime; l.size = size;
            l.speed = speed; l.gravity = gravity; l.radius = radius;
            l.drag = drag; l.intensity = intensity; l.coneAngleDeg = coneAngleDeg;
            l.height = height; l.originHeight = originHeight; l.swirl = swirl;
            l.enabled = true;
            return l;
        }

        EffectDefinition mkDef(const char* name, EffectCategory cat, bool loop, float duration,
                               std::initializer_list<EffectLayer> layers)
        {
            EffectDefinition d;
            d.name = name; d.category = cat; d.loop = loop; d.duration = duration;
            int i = 0;
            for (const auto& l : layers)
            {
                if (i >= kMaxEffectLayers) break;
                d.layers[static_cast<std::size_t>(i++)] = l;
            }
            d.layerCount = i;
            return d;
        }

        using C = EffectCategory;
        using S = EmitterShape;
        constexpr Blend ADD = Blend::Additive;
        constexpr Blend ALP = Blend::Alpha;

        std::vector<EffectDefinition> build_catalog()
        {
            std::vector<EffectDefinition> c;

            // ===== FIRE =====
            c.push_back(mkDef("Fireball impact", C::Fire, false, 0.05f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.7f,0.2f, 0.8f,0.1f,0.0f, 1600,0.4f,0.10f, 7.0f,4.0f,0.2f, 2.0f,1.2f),
                mkLayer(S::Point,  ADD, 1.0f,0.95f,0.7f, 1.0f,0.4f,0.1f, 250,0.12f,0.4f, 0.5f,0.0f,0.05f, 0.0f,1.6f) }));
            c.push_back(mkDef("Flame nova", C::Fire, false, 0.08f, {
                mkLayer(S::Shockwave, ADD, 1.0f,0.6f,0.1f, 0.7f,0.05f,0.0f, 2000,0.5f,0.14f, 9.0f,-1.0f,0.3f, 2.0f,1.2f) }));
            c.push_back(mkDef("Meteor crash", C::Fire, false, 0.1f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.5f,0.1f, 0.5f,0.05f,0.0f, 1800,0.6f,0.16f, 8.0f,5.0f,0.3f, 1.5f,1.3f),
                mkLayer(S::Disc, ALP, 0.3f,0.25f,0.2f, 0.05f,0.05f,0.05f, 120,1.4f,0.5f, 1.5f,-0.6f,0.5f, 0.5f,0.6f) }));
            c.push_back(mkDef("Fire pillar", C::Fire, true, 0.0f, {
                mkLayer(S::Disc, ADD, 1.0f,0.7f,0.2f, 0.7f,0.05f,0.0f, 260,0.8f,0.18f, 2.4f,-1.5f,0.5f, 1.2f,1.0f),
                mkLayer(S::Disc, ALP, 0.25f,0.22f,0.2f, 0.05f,0.05f,0.05f, 40,1.6f,0.35f, 1.6f,-0.8f,0.4f, 0.6f,0.5f) }));
            c.push_back(mkDef("Ember field", C::Fire, true, 0.0f, {
                mkLayer(S::Disc, ADD, 1.0f,0.6f,0.15f, 0.6f,0.1f,0.0f, 40,2.0f,0.05f, 1.0f,-0.6f,0.8f, 0.4f) }));
            c.push_back(mkDef("Flamethrower", C::Fire, true, 0.0f, {
                mkLayer(S::Cone, ADD, 1.0f,0.75f,0.25f, 0.7f,0.1f,0.0f, 400,0.5f,0.14f, 7.0f,-1.0f,0.2f, 1.5f,1.0f, 18.0f) }));

            // ===== WATER =====
            c.push_back(mkDef("Water bolt", C::Water, false, 0.05f, {
                mkLayer(S::Sphere, ALP, 0.6f,0.8f,1.0f, 0.85f,0.95f,1.0f, 900,0.5f,0.10f, 5.0f,9.0f,0.15f, 0.5f,0.8f) }));
            c.push_back(mkDef("Tidal splash", C::Water, false, 0.06f, {
                mkLayer(S::Shockwave, ALP, 0.7f,0.85f,1.0f, 0.9f,0.95f,1.0f, 1400,0.5f,0.12f, 8.0f,4.0f,0.3f, 1.5f,0.8f) }));
            c.push_back(mkDef("Water fountain", C::Water, true, 0.0f, {
                mkLayer(S::Cone, ALP, 0.6f,0.8f,1.0f, 0.85f,0.95f,1.0f, 300,1.4f,0.10f, 6.0f,9.0f,0.2f, 0.0f,0.7f, 14.0f) }));
            c.push_back(mkDef("Bubble stream", C::Water, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.7f,0.9f,1.0f, 0.85f,0.95f,1.0f, 60,2.0f,0.08f, 1.2f,-1.2f,0.3f, 0.3f,0.7f) }));
            c.push_back(mkDef("Healing spring", C::Water, true, 0.0f, {
                mkLayer(S::Ring, ADD, 0.5f,0.9f,1.0f, 0.8f,1.0f,1.0f, 200,1.0f,0.09f, 1.0f,-0.6f,1.0f, 0.0f,0.8f) }));

            // ===== ICE =====
            // Frost nova — large, detailed, very visible icy explosion.
            c.push_back(mkDef("Frost nova", C::Ice, false, 0.14f, {
                mkLayer(S::Shockwave, ADD, 0.8f,0.97f,1.0f, 0.45f,0.7f,1.0f, 2600,0.7f,0.18f, 11.0f,1.0f,0.4f, 1.8f,1.4f),
                mkLayer(S::Sphere, ADD, 0.7f,0.95f,1.0f, 0.5f,0.7f,1.0f, 2200,0.8f,0.13f, 7.0f,2.0f,0.4f, 2.0f,1.2f),
                mkLayer(S::Disc, ALP, 0.85f,0.93f,1.0f, 0.6f,0.75f,0.95f, 400,1.0f,0.5f, 3.0f,0.0f,1.2f, 2.5f,0.5f) }));
            c.push_back(mkDef("Ice shards", C::Ice, false, 0.05f, {
                mkLayer(S::Shockwave, ADD, 0.8f,0.95f,1.0f, 0.55f,0.75f,1.0f, 1400,0.45f,0.10f, 8.0f,2.0f,0.25f, 2.0f,1.0f) }));
            c.push_back(mkDef("Ice spike burst", C::Ice, false, 0.06f, {
                mkLayer(S::Cone, ADD, 0.75f,0.9f,1.0f, 0.5f,0.7f,1.0f, 700,0.6f,0.11f, 6.0f,3.0f,0.15f, 1.0f,1.0f, 16.0f) }));
            // Blizzard — large hailstorm with big ice chunks over an area.
            c.push_back(mkDef("Blizzard", C::Ice, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.9f,0.96f,1.0f, 0.7f,0.82f,0.95f, 260,1.6f,0.30f, 1.0f,16.0f,4.5f, 0.0f,1.0f, 25.0f,1.0f, 12.0f),
                mkLayer(S::Disc, ADD, 0.85f,0.95f,1.0f, 0.6f,0.78f,1.0f, 600,1.4f,0.10f, 1.0f,20.0f,4.5f, 0.0f,0.9f, 25.0f,1.0f, 12.0f),
                mkLayer(S::Disc, ALP, 0.8f,0.88f,0.95f, 0.6f,0.7f,0.85f, 120,2.4f,0.6f, 0.3f,-0.05f,4.0f, 0.6f,0.4f) }));
            c.push_back(mkDef("Frost mist", C::Ice, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.8f,0.9f,1.0f, 0.6f,0.72f,0.9f, 60,2.2f,0.4f, 0.4f,-0.1f,1.1f, 0.8f,0.5f) }));

            // ===== WIND =====
            c.push_back(mkDef("Gust", C::Wind, false, 0.1f, {
                mkLayer(S::Cone, ALP, 0.85f,0.95f,0.9f, 0.7f,0.85f,0.8f, 400,0.6f,0.18f, 7.0f,0.0f,0.2f, 1.5f,0.5f, 35.0f) }));
            c.push_back(mkDef("Wind slash", C::Wind, false, 0.05f, {
                mkLayer(S::Shockwave, ALP, 0.9f,0.97f,0.95f, 0.7f,0.85f,0.8f, 1200,0.4f,0.10f, 11.0f,0.0f,0.3f, 1.5f,0.6f) }));
            c.push_back(mkDef("Air burst", C::Wind, false, 0.05f, {
                mkLayer(S::Sphere, ALP, 0.9f,0.95f,1.0f, 0.75f,0.85f,0.9f, 1000,0.45f,0.14f, 6.0f,0.0f,0.2f, 2.0f,0.5f) }));
            c.push_back(mkDef("Whirlwind", C::Wind, true, 0.0f, {
                mkLayer(S::Cone, ALP, 0.85f,0.92f,0.85f, 0.65f,0.8f,0.75f, 220,1.0f,0.16f, 3.0f,-1.5f,0.6f, 0.3f,0.5f, 30.0f) }));
            c.push_back(mkDef("Leaf swirl", C::Wind, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.5f,0.7f,0.25f, 0.4f,0.5f,0.15f, 50,2.0f,0.10f, 1.2f,-0.6f,0.8f, 0.3f,0.8f) }));

            // ===== EARTH =====
            c.push_back(mkDef("Dust burst", C::Earth, false, 0.06f, {
                mkLayer(S::Sphere, ALP, 0.55f,0.45f,0.32f, 0.3f,0.24f,0.16f, 900,0.7f,0.18f, 4.0f,3.0f,0.25f, 1.2f,0.7f) }));
            c.push_back(mkDef("Earthquake wave", C::Earth, false, 0.08f, {
                mkLayer(S::Shockwave, ALP, 0.5f,0.4f,0.28f, 0.3f,0.22f,0.14f, 1500,0.6f,0.16f, 7.0f,2.0f,0.3f, 1.5f,0.8f) }));
            c.push_back(mkDef("Sand geyser", C::Earth, true, 0.0f, {
                mkLayer(S::Cone, ALP, 0.8f,0.7f,0.45f, 0.5f,0.42f,0.25f, 220,1.2f,0.16f, 5.0f,6.0f,0.2f, 0.4f,0.7f, 20.0f) }));
            c.push_back(mkDef("Dust devil", C::Earth, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.6f,0.5f,0.35f, 0.35f,0.28f,0.18f, 120,1.4f,0.18f, 2.0f,-1.0f,0.5f, 0.3f,0.6f) }));

            // ===== ROCK =====
            c.push_back(mkDef("Rock shatter", C::Rock, false, 0.05f, {
                mkLayer(S::Sphere, ALP, 0.5f,0.5f,0.52f, 0.28f,0.28f,0.3f, 700,0.7f,0.12f, 5.0f,9.0f,0.2f, 0.6f,0.9f) }));
            c.push_back(mkDef("Boulder impact", C::Rock, false, 0.08f, {
                mkLayer(S::Sphere, ALP, 0.45f,0.43f,0.42f, 0.25f,0.24f,0.24f, 1000,0.8f,0.16f, 6.0f,8.0f,0.3f, 0.8f,0.9f),
                mkLayer(S::Shockwave, ALP, 0.55f,0.5f,0.45f, 0.3f,0.27f,0.22f, 900,0.5f,0.14f, 6.0f,1.0f,0.3f, 1.5f,0.7f) }));
            c.push_back(mkDef("Stone shards", C::Rock, false, 0.05f, {
                mkLayer(S::Shockwave, ALP, 0.5f,0.48f,0.48f, 0.3f,0.28f,0.28f, 1200,0.5f,0.10f, 8.0f,3.0f,0.25f, 1.0f,0.9f) }));
            c.push_back(mkDef("Rubble plume", C::Rock, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.4f,0.38f,0.36f, 0.18f,0.17f,0.16f, 60,2.0f,0.4f, 1.5f,-0.7f,0.4f, 0.5f,0.6f) }));

            // ===== LIGHTNING =====
            c.push_back(mkDef("Lightning strike", C::Lightning, false, 0.12f, {
                mkLayer(S::Point, ADD, 0.8f,0.9f,1.0f, 0.4f,0.5f,1.0f, 1200,0.2f,0.07f, 9.0f,3.0f,0.05f, 1.5f,1.5f),
                mkLayer(S::Point, ADD, 0.95f,0.97f,1.0f, 0.6f,0.7f,1.0f, 200,0.1f,0.35f, 0.5f,0.0f,0.05f, 0.0f,1.6f) }));
            c.push_back(mkDef("Thunder burst", C::Lightning, false, 0.05f, {
                mkLayer(S::Shockwave, ADD, 0.85f,0.9f,1.0f, 0.5f,0.6f,1.0f, 1600,0.35f,0.10f, 11.0f,0.0f,0.3f, 2.0f,1.3f) }));
            c.push_back(mkDef("Spark spray", C::Lightning, false, 0.06f, {
                mkLayer(S::Cone, ADD, 0.9f,0.95f,1.0f, 0.5f,0.6f,1.0f, 700,0.3f,0.07f, 8.0f,5.0f,0.1f, 1.5f,1.4f, 22.0f) }));
            c.push_back(mkDef("Static field", C::Lightning, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.7f,0.85f,1.0f, 0.4f,0.5f,1.0f, 120,0.5f,0.06f, 1.5f,0.0f,1.0f, 0.5f,1.2f) }));

            // ===== HOLY =====
            c.push_back(mkDef("Smite", C::Holy, false, 0.1f, {
                mkLayer(S::Line, ADD, 1.0f,0.95f,0.6f, 1.0f,1.0f,0.9f, 600,0.5f,0.12f, 2.0f,-2.0f,0.2f, 0.5f,1.4f, 25.0f, 3.0f),
                mkLayer(S::Disc, ADD, 1.0f,0.9f,0.5f, 1.0f,1.0f,0.85f, 400,0.6f,0.13f, 2.5f,-1.0f,0.7f, 1.0f,1.2f) }));
            c.push_back(mkDef("Divine nova", C::Holy, false, 0.08f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.95f,0.7f, 1.0f,1.0f,0.95f, 1600,0.5f,0.11f, 6.0f,-1.0f,0.2f, 1.5f,1.3f) }));
            c.push_back(mkDef("Holy column", C::Holy, true, 0.0f, {
                mkLayer(S::Line, ADD, 1.0f,0.95f,0.6f, 1.0f,1.0f,0.95f, 160,1.2f,0.13f, 1.2f,-0.5f,0.5f, 0.0f,1.0f, 25.0f, 3.0f) }));
            c.push_back(mkDef("Sanctuary ring", C::Holy, true, 0.0f, {
                mkLayer(S::Ring, ADD, 1.0f,0.92f,0.55f, 1.0f,1.0f,0.9f, 240,1.0f,0.09f, 0.5f,-0.2f,1.6f, 0.0f,1.0f) }));
            c.push_back(mkDef("Blessing motes", C::Holy, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.95f,0.6f, 1.0f,0.85f,0.4f, 30,3.0f,0.05f, 0.25f,-0.1f,1.8f, 0.3f,0.9f) }));
            c.push_back(mkDef("Heal burst", C::Holy, false, 0.5f, {
                mkLayer(S::Disc, ADD, 0.4f,1.0f,0.5f, 0.9f,1.0f,0.8f, 240,1.0f,0.12f, 2.2f,-1.0f,0.7f, 1.0f,1.1f) }));

            // ===== SHADOW =====
            c.push_back(mkDef("Dark burst", C::Shadow, false, 0.07f, {
                mkLayer(S::Sphere, ADD, 0.5f,0.15f,0.7f, 0.1f,0.0f,0.2f, 1400,0.6f,0.13f, 5.0f,-0.5f,0.2f, 1.5f,1.1f) }));
            c.push_back(mkDef("Curse nova", C::Shadow, false, 0.08f, {
                mkLayer(S::Shockwave, ADD, 0.55f,0.2f,0.75f, 0.12f,0.0f,0.22f, 1400,0.6f,0.13f, 8.0f,0.0f,0.3f, 1.5f,1.1f) }));
            c.push_back(mkDef("Shadow flames", C::Shadow, true, 0.0f, {
                mkLayer(S::Disc, ADD, 0.5f,0.15f,0.7f, 0.15f,0.0f,0.25f, 240,0.9f,0.18f, 2.2f,-1.4f,0.45f, 1.1f,1.0f),
                mkLayer(S::Disc, ALP, 0.1f,0.0f,0.15f, 0.0f,0.0f,0.0f, 40,1.5f,0.3f, 1.6f,-0.8f,0.35f, 0.6f,0.5f) }));
            c.push_back(mkDef("Void rift", C::Shadow, true, 0.0f, {
                mkLayer(S::Ring, ADD, 0.4f,0.1f,0.6f, 0.05f,0.0f,0.12f, 220,1.1f,0.11f, 0.6f,-0.2f,1.3f, 0.0f,1.0f),
                mkLayer(S::Disc, ALP, 0.08f,0.0f,0.12f, 0.0f,0.0f,0.0f, 80,0.9f,0.25f, 0.4f,-0.3f,1.0f, 0.5f,0.7f) }));
            c.push_back(mkDef("Shadow tendrils", C::Shadow, true, 0.0f, {
                mkLayer(S::Cone, ALP, 0.3f,0.1f,0.45f, 0.05f,0.0f,0.1f, 120,1.4f,0.16f, 2.0f,-1.0f,0.3f, 0.4f,0.7f, 30.0f) }));

            // ===== NATURE / GRASS =====
            c.push_back(mkDef("Bloom burst", C::Nature, false, 0.4f, {
                mkLayer(S::Disc, ADD, 0.5f,1.0f,0.4f, 0.9f,1.0f,0.6f, 220,0.9f,0.11f, 2.2f,-1.0f,0.6f, 1.0f,1.0f) }));
            c.push_back(mkDef("Vine surge", C::Nature, false, 0.1f, {
                mkLayer(S::Cone, ALP, 0.3f,0.7f,0.2f, 0.15f,0.4f,0.1f, 300,0.7f,0.14f, 5.0f,2.0f,0.2f, 1.0f,0.9f, 18.0f) }));
            c.push_back(mkDef("Pollen motes", C::Nature, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.85f,1.0f,0.4f, 0.5f,0.8f,0.2f, 28,3.0f,0.05f, 0.22f,-0.05f,2.0f, 0.3f,0.85f) }));
            c.push_back(mkDef("Spore cloud", C::Nature, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.5f,0.8f,0.3f, 0.25f,0.45f,0.12f, 60,2.2f,0.35f, 0.4f,-0.1f,1.2f, 0.7f,0.5f) }));
            c.push_back(mkDef("Grass rustle", C::Nature, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.45f,0.7f,0.25f, 0.3f,0.5f,0.15f, 70,1.4f,0.09f, 1.0f,-0.5f,0.9f, 0.4f,0.7f) }));

            // ===== ARCANE / MAGICAL =====
            // Arcane (blue) portal kept under a distinct name.
            c.push_back(mkDef("Portal (arcane)", C::Arcane, true, 0.0f, {
                mkLayer(S::Ring, ADD, 0.55f,0.45f,1.0f, 0.15f,0.25f,0.7f, 220,1.1f,0.10f, 0.8f,-0.2f,1.4f, 0.0f,1.0f),
                mkLayer(S::Disc, ADD, 0.7f,0.6f,1.0f, 0.2f,0.1f,0.6f, 90,0.9f,0.14f, 0.5f,-0.4f,1.1f, 0.0f,0.8f) }));
            // "Portal": the sophisticated fiery vortex used at map gates (matches the
            // retail Shaiya fire portal). Layers: swirling body, bright hot core, and
            // an outward flaming corona. The swirl spins particles around the anchor's
            // local Y axis; the map spawner orients the basis so this plane stands
            // vertical and faces the play area. Args after colours are:
            // spawnRate, lifetime, size, speed, gravity, radius, drag, intensity,
            // coneAngleDeg, height, originHeight, swirl.
            c.push_back(mkDef("Portal", C::Fire, true, 0.0f, {
                mkLayer(S::Disc, ADD, 1.0f,0.55f,0.14f, 0.7f,0.07f,0.0f, 340,0.9f,0.22f, 0.3f,-0.4f,1.5f, 0.6f,1.0f, 25.0f,1.0f,0.0f, 7.0f),
                mkLayer(S::Disc, ADD, 1.0f,0.9f,0.6f, 1.0f,0.4f,0.1f, 150,0.5f,0.17f, 0.2f,-0.2f,0.55f, 0.8f,1.0f, 25.0f,1.0f,0.0f, 9.5f),
                mkLayer(S::Ring, ADD, 1.0f,0.5f,0.1f, 0.6f,0.05f,0.0f, 130,0.7f,0.20f, 1.1f,-0.6f,1.75f, 0.4f,1.0f, 25.0f,1.0f,0.0f, 3.0f) }));
            c.push_back(mkDef("Arcane sigil", C::Arcane, true, 0.0f, {
                mkLayer(S::Ring, ADD, 0.8f,0.4f,1.0f, 0.4f,0.1f,0.8f, 300,0.9f,0.08f, 0.3f,-0.1f,1.6f, 0.0f,1.0f),
                mkLayer(S::Ring, ADD, 0.95f,0.7f,1.0f, 0.5f,0.2f,0.9f, 200,0.9f,0.07f, 0.3f,-0.1f,0.9f, 0.0f,0.9f) }));
            c.push_back(mkDef("Mana burst", C::Arcane, false, 0.07f, {
                mkLayer(S::Sphere, ADD, 0.5f,0.6f,1.0f, 0.2f,0.3f,0.9f, 1400,0.55f,0.11f, 5.5f,-0.5f,0.2f, 1.5f,1.1f) }));
            c.push_back(mkDef("Arcane missiles", C::Arcane, false, 0.15f, {
                mkLayer(S::Cone, ADD, 0.6f,0.5f,1.0f, 0.3f,0.2f,0.9f, 300,0.6f,0.10f, 7.0f,0.0f,0.1f, 1.0f,1.2f, 14.0f) }));
            c.push_back(mkDef("Teleport flash", C::Arcane, false, 0.06f, {
                mkLayer(S::Line, ADD, 0.7f,0.6f,1.0f, 0.95f,0.9f,1.0f, 700,0.4f,0.12f, 1.5f,-2.5f,0.25f, 0.5f,1.3f, 25.0f, 2.5f) }));
            c.push_back(mkDef("Magic shield", C::Arcane, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.4f,0.6f,1.0f, 0.2f,0.4f,0.9f, 260,0.7f,0.07f, 0.2f,0.0f,1.3f, 0.0f,0.8f) }));

            // ===== POISON =====
            c.push_back(mkDef("Venom burst", C::Poison, false, 0.06f, {
                mkLayer(S::Sphere, ALP, 0.5f,1.0f,0.25f, 0.1f,0.35f,0.05f, 900,0.7f,0.14f, 4.5f,2.0f,0.2f, 1.0f,0.8f) }));
            c.push_back(mkDef("Acid splash", C::Poison, false, 0.05f, {
                mkLayer(S::Shockwave, ALP, 0.55f,0.95f,0.3f, 0.15f,0.4f,0.08f, 1200,0.5f,0.11f, 7.0f,3.0f,0.25f, 1.0f,0.8f) }));
            c.push_back(mkDef("Poison cloud", C::Poison, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.5f,0.9f,0.25f, 0.1f,0.3f,0.05f, 70,2.2f,0.4f, 0.4f,-0.15f,1.3f, 0.8f,0.6f) }));
            c.push_back(mkDef("Toxic geyser", C::Poison, true, 0.0f, {
                mkLayer(S::Cone, ALP, 0.5f,1.0f,0.25f, 0.1f,0.35f,0.05f, 200,1.6f,0.22f, 5.0f,6.0f,0.2f, 0.4f,0.7f, 22.0f) }));
            c.push_back(mkDef("Plague mist", C::Poison, true, 0.0f, {
                mkLayer(S::Sphere, ALP, 0.45f,0.7f,0.25f, 0.12f,0.28f,0.06f, 90,2.4f,0.30f, 0.5f,-0.1f,1.4f, 0.7f,0.5f) }));

            // ===== NORMAL =====
            c.push_back(mkDef("Impact", C::Normal, false, 0.06f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.85f,0.4f, 0.9f,0.2f,0.05f, 1400,0.35f,0.08f, 6.0f,8.0f,0.1f, 2.0f,1.2f),
                mkLayer(S::Point, ADD, 1.0f,0.95f,0.7f, 1.0f,0.5f,0.1f, 300,0.12f,0.3f, 0.5f,0.0f,0.05f, 0.0f,1.5f) }));
            c.push_back(mkDef("Shockwave", C::Normal, false, 0.05f, {
                mkLayer(S::Shockwave, ADD, 1.0f,0.95f,0.8f, 1.0f,0.6f,0.2f, 2000,0.4f,0.12f, 10.0f,0.0f,0.3f, 3.0f,1.2f) }));
            c.push_back(mkDef("Blood splatter", C::Normal, false, 0.05f, {
                mkLayer(S::Sphere, ALP, 0.7f,0.05f,0.05f, 0.3f,0.0f,0.0f, 900,0.6f,0.08f, 4.5f,12.0f,0.15f, 1.0f,1.0f) }));
            c.push_back(mkDef("Smoke plume", C::Normal, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.30f,0.30f,0.32f, 0.06f,0.06f,0.07f, 55,2.4f,0.45f, 1.4f,-0.7f,0.3f, 0.5f,0.55f) }));
            c.push_back(mkDef("Sparkle", C::Normal, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 1.0f,1.0f,0.9f, 0.8f,0.8f,0.5f, 40,1.5f,0.05f, 0.5f,-0.2f,0.8f, 0.4f,0.9f) }));
            c.push_back(mkDef("Dust", C::Normal, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.55f,0.52f,0.48f, 0.3f,0.28f,0.25f, 40,2.0f,0.25f, 0.6f,-0.2f,0.7f, 0.5f,0.4f) }));

            // ===== REQUESTED SPELLS =====
            // Fireball — forward fire projectile.
            {
                auto d = mkDef("Fireball", C::Fire, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 1.0f,0.7f,0.2f, 0.8f,0.15f,0.0f, 520,0.35f,0.14f, 0.7f,-0.5f,0.16f, 1.0f,1.2f),
                    mkLayer(S::Sphere, ALP, 0.22f,0.18f,0.16f, 0.0f,0.0f,0.0f, 70,0.6f,0.20f, 0.3f,-0.3f,0.10f, 0.3f,0.5f) });
                d.projectile = true; d.projectileSpeed = 16.0f; d.projectileRange = 32.0f;
                c.push_back(d);
            }
            // Magic missile — simple forward arcane bolt.
            {
                auto d = mkDef("Magic missile", C::Arcane, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 0.5f,0.5f,1.0f, 0.3f,0.2f,0.9f, 460,0.4f,0.12f, 0.5f,0.0f,0.12f, 1.0f,1.2f),
                    mkLayer(S::Sphere, ADD, 0.8f,0.8f,1.0f, 0.4f,0.4f,1.0f, 130,0.5f,0.06f, 1.0f,0.0f,0.15f, 0.5f,1.0f) });
                d.projectile = true; d.projectileSpeed = 15.0f; d.projectileRange = 30.0f;
                c.push_back(d);
            }
            // Frost ball — forward ice projectile.
            {
                auto d = mkDef("Frost ball", C::Ice, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 0.7f,0.95f,1.0f, 0.5f,0.7f,1.0f, 460,0.4f,0.13f, 0.5f,0.5f,0.14f, 1.0f,1.1f),
                    mkLayer(S::Sphere, ALP, 0.8f,0.92f,1.0f, 0.6f,0.75f,0.95f, 90,0.6f,0.18f, 0.3f,0.0f,0.10f, 0.4f,0.6f) });
                d.projectile = true; d.projectileSpeed = 14.0f; d.projectileRange = 28.0f;
                c.push_back(d);
            }
            // Fire bush — a curtain of fire that rings the caster.
            c.push_back(mkDef("Fire bush", C::Fire, true, 0.0f, {
                mkLayer(S::Ring, ADD, 1.0f,0.65f,0.2f, 0.7f,0.1f,0.0f, 360,0.7f,0.16f, 2.0f,-1.2f,1.2f, 1.0f,1.1f),
                mkLayer(S::Ring, ADD, 1.0f,0.5f,0.1f, 0.5f,0.05f,0.0f, 90,1.2f,0.06f, 1.6f,-0.6f,1.3f, 0.5f,1.0f) }));
            // Rock blast — boulders rain from the sky onto the target.
            c.push_back(mkDef("Rock blast", C::Rock, false, 1.0f, {
                mkLayer(S::Disc, ALP, 0.45f,0.42f,0.40f, 0.25f,0.23f,0.22f, 110,1.5f,0.20f, 0.5f,14.0f,1.5f, 0.0f,1.0f, 25.0f, 1.0f, 9.0f),
                mkLayer(S::Disc, ALP, 0.5f,0.46f,0.4f, 0.2f,0.18f,0.16f, 35,1.2f,0.40f, 1.0f,-0.4f,1.4f, 0.5f,0.6f) }));
            // Magic roots — earthen tendrils rise from the ground.
            c.push_back(mkDef("Magic roots", C::Nature, false, 1.2f, {
                mkLayer(S::Disc, ALP, 0.35f,0.22f,0.10f, 0.20f,0.45f,0.10f, 180,1.4f,0.13f, 1.8f,-0.5f,0.8f, 0.6f,1.0f),
                mkLayer(S::Disc, ADD, 0.3f,0.7f,0.15f, 0.15f,0.5f,0.1f, 60,1.2f,0.07f, 2.2f,-0.8f,0.7f, 0.4f,0.9f) }));
            // Electric shock — a lightning bolt strikes from the sky.
            c.push_back(mkDef("Electric shock", C::Lightning, false, 0.18f, {
                mkLayer(S::Line, ADD, 0.85f,0.92f,1.0f, 0.5f,0.6f,1.0f, 2500,0.18f,0.10f, 0.5f,0.0f,0.06f, 0.0f,1.6f, 25.0f, 12.0f),
                mkLayer(S::Disc, ADD, 0.9f,0.95f,1.0f, 0.5f,0.6f,1.0f, 600,0.14f,0.40f, 0.5f,0.0f,0.6f, 0.0f,1.6f),
                mkLayer(S::Shockwave, ADD, 0.8f,0.9f,1.0f, 0.4f,0.5f,1.0f, 1200,0.3f,0.08f, 8.0f,4.0f,0.2f, 2.0f,1.4f) }));

            // ===== BIG / ELABORATE SPELLS =====
            // Tornado — brief swirling wind column at a location.
            c.push_back(mkDef("Tornado", C::Wind, false, 2.5f, {
                mkLayer(S::Disc, ALP, 0.6f,0.55f,0.45f, 0.3f,0.28f,0.22f, 340,1.6f,0.22f, 3.5f,-0.6f,1.0f, 0.0f,0.7f, 25.0f,1.0f,0.0f, 5.0f),
                mkLayer(S::Disc, ALP, 0.5f,0.45f,0.35f, 0.25f,0.22f,0.18f, 180,1.4f,0.14f, 4.0f,-0.8f,0.5f, 0.0f,0.8f, 25.0f,1.0f,0.0f, 7.5f),
                mkLayer(S::Disc, ALP, 0.65f,0.6f,0.5f, 0.35f,0.32f,0.26f, 120,1.3f,0.26f, 2.5f,-0.5f,1.5f, 0.0f,0.6f, 25.0f,1.0f,2.8f, 4.0f) }));
            // Turbulence — giant tornado with lightning crackling inside.
            c.push_back(mkDef("Turbulence", C::Wind, false, 3.5f, {
                mkLayer(S::Disc, ALP, 0.55f,0.55f,0.6f, 0.3f,0.3f,0.34f, 520,2.0f,0.34f, 4.0f,-0.6f,2.2f, 0.0f,0.7f, 25.0f,1.0f,0.0f, 6.0f),
                mkLayer(S::Disc, ALP, 0.5f,0.5f,0.55f, 0.28f,0.28f,0.32f, 280,1.8f,0.20f, 5.0f,-0.7f,1.0f, 0.0f,0.8f, 25.0f,1.0f,0.0f, 9.0f),
                mkLayer(S::Sphere, ADD, 0.8f,0.9f,1.0f, 0.4f,0.5f,1.0f, 200,0.2f,0.13f, 1.0f,0.0f,1.6f, 0.0f,1.6f) }));
            // Meteors — detailed meteorites raining onto a location.
            c.push_back(mkDef("Meteors", C::Fire, false, 1.6f, {
                mkLayer(S::Disc, ADD, 1.0f,0.6f,0.2f, 0.6f,0.1f,0.0f, 95,1.2f,0.30f, 1.0f,16.0f,1.6f, 0.0f,1.2f, 25.0f,1.0f, 13.0f),
                mkLayer(S::Disc, ADD, 1.0f,0.45f,0.1f, 0.4f,0.05f,0.0f, 260,0.6f,0.16f, 0.5f,11.0f,1.6f, 0.0f,1.1f, 25.0f,1.0f, 9.0f),
                mkLayer(S::Disc, ALP, 0.2f,0.16f,0.14f, 0.0f,0.0f,0.0f, 70,1.0f,0.30f, 0.5f,4.0f,1.6f, 0.4f,0.5f, 25.0f,1.0f, 7.0f) }));
            // Infernal fire — huge, detailed fireball burst with black smoke.
            c.push_back(mkDef("Infernal fire", C::Fire, false, 0.12f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.55f,0.15f, 0.7f,0.1f,0.0f, 1900,0.7f,0.40f, 5.0f,-1.0f,0.8f, 1.2f,1.3f),
                mkLayer(S::Sphere, ALP, 0.10f,0.08f,0.08f, 0.0f,0.0f,0.0f, 420,1.0f,0.50f, 3.0f,-0.8f,0.9f, 1.0f,0.8f),
                mkLayer(S::Sphere, ADD, 1.0f,0.7f,0.2f, 0.6f,0.1f,0.0f, 520,0.9f,0.10f, 7.0f,5.0f,0.5f, 1.0f,1.2f) }));
            // Earthquake — ground heave: dust shockwave + rising dust + popping rocks.
            c.push_back(mkDef("Earthquake", C::Earth, false, 1.4f, {
                mkLayer(S::Shockwave, ALP, 0.5f,0.42f,0.3f, 0.3f,0.24f,0.16f, 1400,0.8f,0.22f, 7.0f,1.0f,0.5f, 1.0f,0.8f),
                mkLayer(S::Disc, ALP, 0.55f,0.48f,0.36f, 0.3f,0.26f,0.2f, 220,1.5f,0.40f, 1.5f,-0.4f,2.5f, 0.4f,0.6f),
                mkLayer(S::Disc, ALP, 0.45f,0.42f,0.4f, 0.25f,0.23f,0.22f, 120,1.0f,0.16f, 4.0f,12.0f,2.0f, 0.0f,0.9f) }));
            // Chain lightning — many strikes battering a large area.
            c.push_back(mkDef("Chain lightning", C::Lightning, false, 0.7f, {
                mkLayer(S::Disc, ADD, 0.85f,0.92f,1.0f, 0.5f,0.6f,1.0f, 1200,0.25f,0.08f, 1.0f,30.0f,3.0f, 0.0f,1.5f, 25.0f,1.0f, 9.0f),
                mkLayer(S::Disc, ADD, 0.9f,0.95f,1.0f, 0.5f,0.6f,1.0f, 260,0.12f,0.40f, 0.5f,0.0f,3.0f, 0.0f,1.6f),
                mkLayer(S::Sphere, ADD, 0.8f,0.9f,1.0f, 0.4f,0.5f,1.0f, 320,0.3f,0.07f, 6.0f,5.0f,2.5f, 1.5f,1.4f) }));
            // Hailstorm — falling hail over an area (looping weather prop).
            c.push_back(mkDef("Hailstorm", C::Ice, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.85f,0.92f,1.0f, 0.7f,0.8f,0.95f, 220,1.4f,0.12f, 1.0f,16.0f,3.5f, 0.0f,1.0f, 25.0f,1.0f, 10.0f),
                mkLayer(S::Disc, ADD, 0.85f,0.95f,1.0f, 0.6f,0.78f,1.0f, 300,1.2f,0.06f, 1.0f,22.0f,3.5f, 0.0f,0.9f, 25.0f,1.0f, 10.0f) }));
            // Elemental shock — fire/ice/nature clashing in a zone.
            c.push_back(mkDef("Elemental shock", C::Arcane, false, 0.15f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.5f,0.1f, 0.6f,0.1f,0.0f, 900,0.5f,0.14f, 6.0f,1.0f,0.5f, 1.5f,1.2f),
                mkLayer(S::Sphere, ADD, 0.6f,0.9f,1.0f, 0.4f,0.6f,1.0f, 900,0.5f,0.13f, 6.0f,1.0f,0.5f, 1.5f,1.2f),
                mkLayer(S::Shockwave, ADD, 0.5f,1.0f,0.3f, 0.2f,0.5f,0.1f, 1000,0.5f,0.12f, 8.0f,0.0f,0.3f, 2.0f,1.2f) }));

            // ===== ELEMENTAL CHOIRS (big arcing projectiles) =====
            {
                auto d = mkDef("Fire choir", C::Fire, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 1.0f,0.6f,0.18f, 0.7f,0.12f,0.0f, 700,0.45f,0.28f, 0.8f,-0.5f,0.4f, 1.0f,1.3f),
                    mkLayer(S::Sphere, ALP, 0.2f,0.15f,0.14f, 0.0f,0.0f,0.0f, 90,0.7f,0.30f, 0.4f,-0.3f,0.25f, 0.3f,0.6f) });
                d.projectile = true; d.projectileSpeed = 13.0f; d.projectileRange = 40.0f; d.projectileGravity = 6.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Water choir", C::Water, true, 0.0f, {
                    mkLayer(S::Sphere, ALP, 0.55f,0.8f,1.0f, 0.8f,0.92f,1.0f, 700,0.5f,0.26f, 0.8f,0.0f,0.4f, 0.6f,0.9f),
                    mkLayer(S::Sphere, ADD, 0.7f,0.9f,1.0f, 0.4f,0.6f,1.0f, 180,0.5f,0.12f, 1.0f,0.0f,0.45f, 0.5f,1.0f) });
                d.projectile = true; d.projectileSpeed = 12.0f; d.projectileRange = 38.0f; d.projectileGravity = 6.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Wind choir", C::Wind, true, 0.0f, {
                    mkLayer(S::Sphere, ALP, 0.8f,0.95f,0.85f, 0.6f,0.8f,0.7f, 600,0.5f,0.26f, 1.0f,0.0f,0.4f, 0.5f,0.9f, 25.0f,1.0f,0.0f, 4.0f),
                    mkLayer(S::Sphere, ADD, 0.85f,0.95f,0.9f, 0.6f,0.8f,0.7f, 160,0.5f,0.12f, 1.2f,0.0f,0.45f, 0.5f,1.0f) });
                d.projectile = true; d.projectileSpeed = 15.0f; d.projectileRange = 42.0f; d.projectileGravity = 4.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Earth choir", C::Earth, true, 0.0f, {
                    mkLayer(S::Sphere, ALP, 0.5f,0.42f,0.3f, 0.3f,0.25f,0.18f, 650,0.5f,0.28f, 0.8f,0.0f,0.4f, 0.5f,1.0f),
                    mkLayer(S::Sphere, ADD, 0.6f,0.5f,0.35f, 0.35f,0.28f,0.2f, 140,0.5f,0.12f, 1.0f,0.0f,0.45f, 0.5f,0.9f) });
                d.projectile = true; d.projectileSpeed = 12.0f; d.projectileRange = 36.0f; d.projectileGravity = 8.0f;
                c.push_back(d);
            }

            // ===== ELEMENTAL SHOCKS (large turbulent columns) =====
            c.push_back(mkDef("Fire shock", C::Fire, false, 2.2f, {
                mkLayer(S::Disc, ADD, 1.0f,0.6f,0.18f, 0.7f,0.1f,0.0f, 420,0.9f,0.22f, 3.5f,-1.2f,0.8f, 0.6f,1.2f, 25.0f,1.0f,0.0f, 4.5f),
                mkLayer(S::Disc, ALP, 0.2f,0.15f,0.13f, 0.0f,0.0f,0.0f, 70,1.3f,0.35f, 2.0f,-0.7f,0.7f, 0.4f,0.6f, 25.0f,1.0f,0.0f, 3.0f) }));
            c.push_back(mkDef("Water shock", C::Water, false, 2.2f, {
                mkLayer(S::Disc, ALP, 0.55f,0.8f,1.0f, 0.8f,0.92f,1.0f, 420,0.9f,0.20f, 3.5f,-1.0f,0.8f, 0.5f,0.8f, 25.0f,1.0f,0.0f, 5.0f),
                mkLayer(S::Disc, ADD, 0.7f,0.9f,1.0f, 0.4f,0.6f,1.0f, 120,0.9f,0.10f, 4.0f,-1.0f,0.6f, 0.5f,0.9f, 25.0f,1.0f,0.0f, 6.0f) }));
            c.push_back(mkDef("Wind shock", C::Wind, false, 2.2f, {
                mkLayer(S::Disc, ALP, 0.8f,0.92f,0.85f, 0.6f,0.78f,0.72f, 420,1.0f,0.22f, 3.5f,-1.0f,0.9f, 0.4f,0.7f, 25.0f,1.0f,0.0f, 6.5f) }));
            c.push_back(mkDef("Earth shock", C::Earth, false, 2.2f, {
                mkLayer(S::Disc, ALP, 0.5f,0.42f,0.3f, 0.3f,0.25f,0.18f, 420,1.0f,0.24f, 3.0f,-1.0f,0.9f, 0.4f,0.7f, 25.0f,1.0f,0.0f, 4.5f),
                mkLayer(S::Disc, ALP, 0.45f,0.42f,0.4f, 0.25f,0.23f,0.22f, 100,0.9f,0.14f, 3.5f,8.0f,0.8f, 0.0f,0.8f) }));

            // ===== PHYSICAL (weapon / melee) =====
            c.push_back(mkDef("Wind spin", C::Normal, false, 0.5f, {
                mkLayer(S::Ring, ALP, 0.9f,0.95f,1.0f, 0.7f,0.8f,0.85f, 600,0.4f,0.16f, 0.2f,0.0f,1.3f, 0.0f,0.9f, 25.0f,1.0f,0.6f, 12.0f),
                mkLayer(S::Ring, ADD, 0.95f,0.97f,1.0f, 0.7f,0.85f,0.9f, 300,0.4f,0.10f, 0.2f,0.0f,1.1f, 0.0f,0.9f, 25.0f,1.0f,0.6f, 14.0f) }));
            c.push_back(mkDef("Deadly strike", C::Normal, false, 0.5f, {
                mkLayer(S::Shockwave, ADD, 1.0f,0.9f,0.8f, 1.0f,0.3f,0.2f, 1600,0.3f,0.12f, 9.0f,0.0f,0.2f, 2.0f,1.3f),
                mkLayer(S::Sphere, ADD, 1.0f,0.85f,0.5f, 0.9f,0.2f,0.1f, 800,0.3f,0.07f, 7.0f,6.0f,0.2f, 1.5f,1.2f) }));
            c.push_back(mkDef("Great strike", C::Normal, false, 0.08f, {
                mkLayer(S::Shockwave, ADD, 1.0f,0.95f,0.8f, 1.0f,0.6f,0.2f, 2200,0.45f,0.16f, 11.0f,0.0f,0.3f, 2.0f,1.4f),
                mkLayer(S::Sphere, ADD, 1.0f,0.9f,0.6f, 0.9f,0.3f,0.1f, 1200,0.4f,0.10f, 8.0f,8.0f,0.2f, 1.5f,1.3f),
                mkLayer(S::Disc, ALP, 0.5f,0.48f,0.45f, 0.25f,0.24f,0.22f, 200,0.6f,0.30f, 2.0f,-0.3f,0.8f, 0.4f,0.6f) }));
            c.push_back(mkDef("Ground shock", C::Earth, false, 0.4f, {
                mkLayer(S::Shockwave, ALP, 0.5f,0.42f,0.3f, 0.3f,0.24f,0.16f, 1400,0.6f,0.20f, 8.0f,1.0f,0.4f, 1.0f,0.8f),
                mkLayer(S::Shockwave, ADD, 0.7f,0.85f,1.0f, 0.4f,0.5f,1.0f, 1000,0.4f,0.10f, 9.0f,0.0f,0.3f, 1.5f,1.4f),
                mkLayer(S::Disc, ADD, 0.8f,0.9f,1.0f, 0.4f,0.5f,1.0f, 300,0.2f,0.30f, 0.5f,0.0f,0.8f, 0.0f,1.4f) }));

            // ===== ARCANE UTILITY =====
            c.push_back(mkDef("Hypnosis", C::Arcane, false, 1.6f, {
                mkLayer(S::Disc, ADD, 0.6f,0.3f,0.9f, 0.3f,0.1f,0.6f, 50,1.6f,0.08f, 0.6f,-0.2f,0.6f, 0.3f,0.6f),
                mkLayer(S::Ring, ADD, 0.7f,0.4f,1.0f, 0.35f,0.15f,0.7f, 60,1.2f,0.07f, 0.4f,-0.15f,0.8f, 0.0f,0.4f) }));
            c.push_back(mkDef("Magic veil", C::Arcane, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.6f,0.4f,1.0f, 0.3f,0.15f,0.8f, 240,0.8f,0.08f, 0.15f,0.0f,1.2f, 0.0f,0.8f) }));
            c.push_back(mkDef("Dispel", C::Arcane, false, 0.9f, {
                mkLayer(S::Disc, ADD, 0.8f,0.75f,1.0f, 0.6f,0.5f,0.95f, 160,0.8f,0.07f, 1.5f,-0.6f,0.5f, 0.5f,0.8f),
                mkLayer(S::Ring, ADD, 0.85f,0.8f,1.0f, 0.6f,0.55f,0.95f, 90,0.7f,0.06f, 0.6f,-0.3f,0.6f, 0.0f,0.6f) }));
            c.push_back(mkDef("Dread visage", C::Shadow, false, 2.0f, {
                mkLayer(S::Disc, ALP, 0.2f,0.05f,0.3f, 0.0f,0.0f,0.05f, 400,1.4f,0.30f, 2.0f,-0.4f,1.2f, 0.0f,0.7f, 25.0f,1.0f,0.0f, 6.0f),
                mkLayer(S::Sphere, ADD, 0.4f,0.1f,0.6f, 0.1f,0.0f,0.2f, 220,1.0f,0.16f, 1.2f,0.0f,0.8f, 0.3f,0.9f),
                mkLayer(S::Shockwave, ADD, 0.5f,0.15f,0.7f, 0.1f,0.0f,0.2f, 200,0.8f,0.14f, 7.0f,0.0f,0.3f, 1.5f,1.0f) }));
            c.push_back(mkDef("Poison shot", C::Poison, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.5f,0.95f,0.3f, 0.2f,0.5f,0.1f, 220,0.35f,0.07f, 0.4f,0.0f,0.06f, 1.0f,0.9f),
                mkLayer(S::Sphere, ALP, 0.4f,0.7f,0.2f, 0.1f,0.3f,0.05f, 70,0.4f,0.05f, 0.3f,0.0f,0.05f, 0.5f,0.6f) }));
            {
                auto& d = c.back();  // configure Poison shot as a small projectile
                d.projectile = true; d.projectileSpeed = 16.0f; d.projectileRange = 28.0f; d.projectileGravity = 1.5f;
            }

            // ===== ELEMENTAL AURAS / BARRIERS =====
            c.push_back(mkDef("Fire aura", C::Fire, true, 0.0f, {
                mkLayer(S::Ring, ADD, 1.0f,0.6f,0.2f, 0.6f,0.1f,0.0f, 130,0.7f,0.10f, 1.2f,-0.8f,0.7f, 0.5f,0.8f) }));
            c.push_back(mkDef("Frost barrier", C::Ice, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 0.7f,0.92f,1.0f, 0.5f,0.7f,1.0f, 240,0.8f,0.09f, 0.15f,0.0f,1.2f, 0.0f,0.9f),
                mkLayer(S::Disc, ALP, 0.8f,0.92f,1.0f, 0.6f,0.75f,0.95f, 60,1.2f,0.10f, 0.3f,0.6f,1.2f, 0.4f,0.5f) }));

            // ===== HOLY / SUPPORT =====
            c.push_back(mkDef("Healing", C::Holy, true, 0.0f, {
                mkLayer(S::Ring, ADD, 1.0f,0.92f,0.55f, 1.0f,1.0f,0.9f, 200,1.0f,0.08f, 0.3f,-0.2f,1.0f, 0.0f,0.9f),
                mkLayer(S::Disc, ADD, 0.7f,1.0f,0.6f, 1.0f,1.0f,0.85f, 60,1.4f,0.06f, 1.2f,-0.6f,0.7f, 0.3f,0.8f) }));
            c.push_back(mkDef("Recovery", C::Holy, true, 0.0f, {
                mkLayer(S::Ring, ADD, 1.0f,0.92f,0.55f, 1.0f,1.0f,0.9f, 420,1.0f,0.10f, 0.4f,-0.2f,1.4f, 0.0f,1.1f),
                mkLayer(S::Line, ADD, 1.0f,0.95f,0.6f, 1.0f,1.0f,0.95f, 220,1.2f,0.12f, 1.6f,-0.4f,0.5f, 0.0f,1.1f, 25.0f,3.0f),
                mkLayer(S::Disc, ADD, 1.0f,1.0f,0.8f, 1.0f,0.9f,0.5f, 160,1.4f,0.07f, 2.2f,-0.7f,1.0f, 0.4f,1.0f) }));
            c.push_back(mkDef("Aurora veil", C::Holy, true, 0.0f, {
                mkLayer(S::Ring, ADD, 0.3f,1.0f,0.6f, 0.2f,0.7f,1.0f, 300,1.4f,0.12f, 1.5f,-0.4f,3.0f, 0.0f,0.9f),
                mkLayer(S::Ring, ADD, 0.6f,0.4f,1.0f, 0.3f,0.7f,1.0f, 200,1.4f,0.10f, 1.4f,-0.4f,3.0f, 0.0f,0.8f) }));
            c.push_back(mkDef("Healing rain", C::Holy, true, 0.0f, {
                mkLayer(S::Disc, ALP, 0.55f,0.8f,1.0f, 0.8f,0.92f,1.0f, 200,1.2f,0.08f, 1.0f,9.0f,3.0f, 0.0f,0.8f, 25.0f,1.0f, 8.0f),
                mkLayer(S::Disc, ADD, 0.5f,0.85f,1.0f, 0.8f,0.95f,1.0f, 60,1.0f,0.20f, 0.5f,-0.2f,2.8f, 0.3f,0.6f) }));
            c.push_back(mkDef("Halo", C::Holy, false, 0.5f, {
                mkLayer(S::Shockwave, ADD, 1.0f,0.95f,0.6f, 1.0f,1.0f,0.9f, 2500,0.8f,0.16f, 9.0f,0.0f,0.5f, 1.0f,1.3f) }));
            c.push_back(mkDef("Holy shield", C::Holy, true, 0.0f, {
                mkLayer(S::Sphere, ADD, 1.0f,0.9f,0.5f, 1.0f,1.0f,0.85f, 240,0.8f,0.09f, 0.15f,0.0f,1.2f, 0.0f,0.9f) }));
            c.push_back(mkDef("Resurrection", C::Holy, false, 1.6f, {
                mkLayer(S::Line, ADD, 1.0f,0.95f,0.6f, 1.0f,1.0f,0.95f, 400,1.2f,0.14f, 2.0f,-0.5f,0.5f, 0.0f,1.2f, 25.0f,4.0f),
                mkLayer(S::Ring, ADD, 1.0f,0.92f,0.55f, 1.0f,1.0f,0.9f, 300,1.2f,0.10f, 0.4f,-0.2f,1.5f, 0.0f,1.1f),
                mkLayer(S::Disc, ADD, 1.0f,1.0f,0.8f, 1.0f,0.9f,0.5f, 220,1.4f,0.08f, 2.5f,-0.8f,1.0f, 0.4f,1.1f) }));

            // ===== ELEMENTAL STREAMS (channelled forward, Comet-Azur style) =====
            {
                auto d = mkDef("Fire stream", C::Fire, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 1.0f,0.6f,0.2f, 0.7f,0.1f,0.0f, 1300,0.5f,0.34f, 0.6f,-0.4f,0.45f, 1.0f,1.3f),
                    mkLayer(S::Sphere, ALP, 0.2f,0.15f,0.13f, 0.0f,0.0f,0.0f, 120,0.7f,0.30f, 0.4f,-0.3f,0.3f, 0.3f,0.6f) });
                d.projectile = true; d.projectileSpeed = 24.0f; d.projectileRange = 60.0f; d.projectileGravity = 0.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Water stream", C::Water, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 0.5f,0.8f,1.0f, 0.3f,0.5f,1.0f, 1300,0.5f,0.32f, 0.6f,0.0f,0.45f, 0.8f,1.2f),
                    mkLayer(S::Sphere, ALP, 0.7f,0.9f,1.0f, 0.85f,0.95f,1.0f, 120,0.6f,0.22f, 0.4f,0.0f,0.3f, 0.4f,0.7f) });
                d.projectile = true; d.projectileSpeed = 24.0f; d.projectileRange = 60.0f; d.projectileGravity = 0.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Wind stream", C::Wind, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 0.8f,0.95f,0.85f, 0.6f,0.8f,0.7f, 1200,0.5f,0.30f, 0.6f,0.0f,0.45f, 0.6f,1.1f, 25.0f,1.0f,0.0f, 4.0f) });
                d.projectile = true; d.projectileSpeed = 27.0f; d.projectileRange = 64.0f; d.projectileGravity = 0.0f;
                c.push_back(d);
            }
            {
                auto d = mkDef("Earth stream", C::Earth, true, 0.0f, {
                    mkLayer(S::Sphere, ADD, 0.55f,0.45f,0.3f, 0.3f,0.24f,0.16f, 1200,0.5f,0.32f, 0.6f,0.0f,0.45f, 0.6f,1.1f),
                    mkLayer(S::Sphere, ALP, 0.5f,0.46f,0.4f, 0.25f,0.22f,0.2f, 120,0.7f,0.26f, 0.4f,0.0f,0.3f, 0.3f,0.6f) });
                d.projectile = true; d.projectileSpeed = 22.0f; d.projectileRange = 56.0f; d.projectileGravity = 0.0f;
                c.push_back(d);
            }

            return c;
        }
    }

    const char* category_name(EffectCategory category)
    {
        switch (category)
        {
        case EffectCategory::Normal:    return "Normal";
        case EffectCategory::Fire:      return "Fire";
        case EffectCategory::Water:     return "Water";
        case EffectCategory::Ice:       return "Ice";
        case EffectCategory::Wind:      return "Wind";
        case EffectCategory::Earth:     return "Earth";
        case EffectCategory::Rock:      return "Rock";
        case EffectCategory::Lightning: return "Lightning";
        case EffectCategory::Holy:      return "Holy";
        case EffectCategory::Shadow:    return "Shadow";
        case EffectCategory::Nature:    return "Nature";
        case EffectCategory::Arcane:    return "Arcane";
        case EffectCategory::Poison:    return "Poison";
        default:                        return "?";
        }
    }

    const std::vector<EffectDefinition>& preset_catalog()
    {
        static const std::vector<EffectDefinition> catalog = build_catalog();
        return catalog;
    }

    EffectDefinition preset_impact()
    {
        return mkDef("Impact", EffectCategory::Normal, false, 0.06f, {
            mkLayer(EmitterShape::Sphere, Blend::Additive, 1.0f,0.85f,0.4f, 0.9f,0.2f,0.05f,
                    1400,0.35f,0.08f, 6.0f,8.0f,0.1f, 2.0f,1.2f),
            mkLayer(EmitterShape::Point, Blend::Additive, 1.0f,0.95f,0.7f, 1.0f,0.5f,0.1f,
                    300,0.12f,0.3f, 0.5f,0.0f,0.05f, 0.0f,1.5f) });
    }
}
