#include "runtime/effect_particle_system.h"
#include "runtime/phoenix_runtime.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phoenix::runtime
{
    namespace
    {
        using phoenix::world::EftEffect;
        using phoenix::world::EftMesh;
        using phoenix::world::EftScaleKeyframe;
        using phoenix::renderer::TerrainVertex;
        using phoenix::renderer::EffectParticleInstance;
        using phoenix::renderer::EffectParticleBatch;

        struct Color4 { float r{1}, g{1}, b{1}, a{1}; };

        float lerpf(float a, float b, float t) { return a + (b - a) * t; }
        float clampf(float v, float lo, float hi) { return std::min(hi, std::max(lo, v)); }
        float fract(float v) { return v - std::floor(v); }
        float random_value(float seed) { return fract(std::sin(seed * 12.9898f) * 43758.5453f); }
        float random_range(float minV, float maxV, float seed)
        {
            if (maxV < minV) return lerpf(maxV, minV, random_value(seed));
            return lerpf(minV, maxV, random_value(seed));
        }

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

        // v*cosA + cross(k,v)*sinA + k*dot(k,v)*(1-cosA) — same formula
        // static_object.vert uses for MANI spin.
        void rodrigues(float v[3], const float k[3], float cosA, float sinA)
        {
            const float cr[3]{
                k[1] * v[2] - k[2] * v[1],
                k[2] * v[0] - k[0] * v[2],
                k[0] * v[1] - k[1] * v[0],
            };
            const float dot = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
            for (int i = 0; i < 3; ++i)
                v[i] = v[i] * cosA + cr[i] * sinA + k[i] * dot * (1.0f - cosA);
        }

        bool axis_vector(std::int32_t axis, const float right[3], const float up[3], const float forward[3], float out[3])
        {
            if (axis == 1) { out[0] = right[0]; out[1] = right[1]; out[2] = right[2]; return true; }
            if (axis == 2) { out[0] = up[0]; out[1] = up[1]; out[2] = up[2]; return true; }
            if (axis == 3) { out[0] = forward[0]; out[1] = forward[1]; out[2] = forward[2]; return true; }
            return false;
        }

        void rotate_xz(float v[3], float radians)
        {
            const float c = std::cos(radians), s = std::sin(radians);
            const float x = c * v[0] - s * v[2];
            const float z = s * v[0] + c * v[2];
            v[0] = x; v[2] = z;
        }
        void rotate_xy(float v[3], float radians)
        {
            const float c = std::cos(radians), s = std::sin(radians);
            const float x = s * v[1] + c * v[0];
            const float y = c * v[1] - s * v[0];
            v[0] = x; v[1] = y;
        }
        void rotate_yz(float v[3], float radians)
        {
            const float c = std::cos(radians), s = std::sin(radians);
            const float y = s * v[2] + c * v[1];
            const float z = c * v[2] - s * v[1];
            v[1] = y; v[2] = z;
        }

        void random_velocity(const EftEffect& e, float seed, float out[3])
        {
            out[0] = random_range(e.velocityMin[0], e.velocityMax[0], seed + 3.1f);
            out[1] = random_range(e.velocityMin[1], e.velocityMax[1], seed + 7.7f);
            out[2] = random_range(e.velocityMin[2], e.velocityMax[2], seed + 11.3f);
        }

        bool emits_particles(const EftEffect& e)
        {
            return std::max(e.emitRateMin, e.emitRateMax) > 0.0f && std::max(e.lifeMin, e.lifeMax) > 0.0f;
        }

        bool is_one_shot_emitter(const EftEffect& e)
        {
            return !e.loop && std::abs(e.emitRateMin - 1.0f) < 0.0001f && std::abs(e.emitRateMax - 1.0f) < 0.0001f;
        }

        float sampled_particle_life(const EftEffect& e, float seed)
        {
            const float mn = std::max(0.0f, e.lifeMin);
            const float mx = std::max(mn, e.lifeMax);
            if (mx <= 0.0f) return std::numeric_limits<float>::infinity();
            return lerpf(mn, mx, random_value(seed + 71.3f));
        }

        float average_emission_rate(const EftEffect& e)
        {
            return std::max(0.0f, (e.emitRateMin + e.emitRateMax) * 0.5f);
        }

        int effect_particle_count(const EftEffect& e)
        {
            if (!emits_particles(e)) return 1;
            if (is_one_shot_emitter(e)) return 1;
            const float averageRate = average_emission_rate(e);
            const float averageLife = std::max(1.0f / 30.0f, (std::max(0.0f, e.lifeMin) + std::max(0.0f, e.lifeMax)) * 0.5f);
            const float emissionWindow = e.loop ? averageLife : std::max(averageLife, e.emitterDuration);
            return static_cast<int>(std::floor(clampf(std::ceil(averageRate * emissionWindow), 1.0f, 200.0f)));
        }

        float initial_rotation(const EftEffect& e, float seed)
        {
            const float mn = static_cast<float>(e.initialRotationMinDegrees);
            const float mx = static_cast<float>(e.initialRotationMaxDegrees);
            const float amount = random_value(seed + 103.7f);
            float degrees;
            if (mx >= mn) degrees = lerpf(mn, mx, amount);
            else if (amount < 0.5f) degrees = lerpf(0.0f, mx, amount * 2.0f);
            else degrees = lerpf(mn, 360.0f, (amount - 0.5f) * 2.0f);
            return degrees * 3.14159265358979323846f / 180.0f;
        }

        Color4 color_at(const EftEffect& e, float time)
        {
            const auto& frames = e.colorFrames;
            if (frames.empty()) return {};
            if (time <= frames.front().time)
                return { frames.front().r, frames.front().g, frames.front().b, frames.front().a };
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                if (time <= frames[i].time)
                {
                    const auto& prev = frames[i - 1];
                    const auto& next = frames[i];
                    const float amount = clampf((time - prev.time) / std::max(0.0001f, next.time - prev.time), 0.0f, 1.0f);
                    return {
                        lerpf(prev.r, next.r, amount), lerpf(prev.g, next.g, amount),
                        lerpf(prev.b, next.b, amount), lerpf(prev.a, next.a, amount),
                    };
                }
            }
            const auto& last = frames.back();
            return { last.r, last.g, last.b, last.a };
        }

        float scale_at(const EftEffect& e, float time, float seed)
        {
            const auto& frames = e.scaleFrames;
            if (frames.empty()) return 0.3f;
            const auto value = [&](const EftScaleKeyframe& frame, std::size_t index) {
                return lerpf(frame.min, frame.max, random_value(seed + 157.3f + static_cast<float>(index) * 23.7f));
            };
            if (time <= frames.front().time) return value(frames.front(), 0);
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                if (time <= frames[i].time)
                {
                    const float amount = clampf(
                        (time - frames[i - 1].time) / std::max(0.0001f, frames[i].time - frames[i - 1].time), 0.0f, 1.0f);
                    return lerpf(value(frames[i - 1], i - 1), value(frames[i], i), amount);
                }
            }
            return value(frames.back(), frames.size() - 1);
        }

        float velocity_scale_at(const EftEffect& e, float time)
        {
            const auto& frames = e.velocityScaleFrames;
            if (frames.empty()) return 0.0f;
            if (time <= frames.front().time) return frames.front().value;
            for (std::size_t i = 1; i < frames.size(); ++i)
            {
                if (time <= frames[i].time)
                {
                    const auto& prev = frames[i - 1];
                    const auto& next = frames[i];
                    return lerpf(prev.value, next.value,
                        clampf((time - prev.time) / std::max(0.0001f, next.time - prev.time), 0.0f, 1.0f));
                }
            }
            return frames.back().value;
        }

        float positive_mod(float value, float divisor)
        {
            return std::fmod(std::fmod(value, divisor) + divisor, divisor);
        }

        float interpolation_amount(float previous, float next, float value)
        {
            return clampf((value - previous) / std::max(0.0001f, next - previous), 0.0f, 1.0f);
        }

        // Which two keyframes (and blend amount) to sample at `tick`, mirroring
        // the reference's sample3de(). `valid=false` means "use the mesh's base
        // (unanimated) vertices" — either no frames, or a single static frame.
        struct MeshSample { std::size_t current{}; std::size_t next{}; float amount{}; bool valid{}; };

        MeshSample sample_mesh_frame_index(const EftMesh& mesh, float tick)
        {
            if (mesh.frames.empty())
                return { 0, 0, 0.0f, false };
            if (mesh.frames.size() == 1 || mesh.maxKeyframe == 0)
                return { 0, 0, 0.0f, true };

            const float period = static_cast<float>(mesh.maxKeyframe + 1);
            const float localTick = positive_mod(std::max(0.0f, tick), period);
            const float firstKey = static_cast<float>(mesh.frames.front().key);
            if (localTick < firstKey)
            {
                const auto last = mesh.frames.size() - 1;
                return { last, 0, interpolation_amount(static_cast<float>(mesh.frames[last].key) - period, firstKey, localTick), true };
            }
            for (std::size_t index = 1; index < mesh.frames.size(); ++index)
            {
                const float nextKey = static_cast<float>(mesh.frames[index].key);
                if (localTick < nextKey)
                {
                    return { index - 1, index,
                        interpolation_amount(static_cast<float>(mesh.frames[index - 1].key), nextKey, localTick), true };
                }
            }
            const auto last = mesh.frames.size() - 1;
            return { last, 0, interpolation_amount(static_cast<float>(mesh.frames[last].key), firstKey + period, localTick), true };
        }

        void sample_mesh_vertex(const EftMesh& mesh, const MeshSample& sample, std::size_t index,
            float outPosition[3], float outUv[2])
        {
            if (!sample.valid)
            {
                const auto& v = mesh.vertices[index];
                outPosition[0] = v.position[0]; outPosition[1] = v.position[1]; outPosition[2] = v.position[2];
                outUv[0] = v.uv[0]; outUv[1] = v.uv[1];
                return;
            }
            const auto& cur = mesh.frames[sample.current].vertices[index];
            const auto& nxt = mesh.frames[sample.next].vertices[index];
            for (int i = 0; i < 3; ++i) outPosition[i] = lerpf(cur.position[i], nxt.position[i], sample.amount);
            outUv[0] = lerpf(cur.uv[0], nxt.uv[0], sample.amount);
            outUv[1] = lerpf(cur.uv[1], nxt.uv[1], sample.amount);
        }

        void apply_velocity_mode(float position[3], float velocity[3], const EftEffect& e, float seed, float age, float dt)
        {
            const float swirl = e.swirlSpeed * dt;
            switch (e.velocityMode)
            {
                case 1:
                {
                    rotate_xz(velocity, swirl);
                    float extra[3]; random_velocity(e, seed + age * 313.1f, extra);
                    for (int i = 0; i < 3; ++i) velocity[i] += extra[i] * dt;
                    break;
                }
                case 2:
                {
                    rotate_xy(velocity, swirl);
                    float extra[3]; random_velocity(e, seed + age * 313.1f, extra);
                    for (int i = 0; i < 3; ++i) velocity[i] += extra[i] * dt;
                    break;
                }
                case 3:
                {
                    rotate_yz(velocity, swirl);
                    float extra[3]; random_velocity(e, seed + age * 313.1f, extra);
                    for (int i = 0; i < 3; ++i) velocity[i] += extra[i] * dt;
                    break;
                }
                default:
                    for (int i = 0; i < 3; ++i) position[i] += velocity[i] * dt;
                    break;
            }
        }

        void compute_orientation_basis(std::int32_t baseAxis,
            const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
            const float placementRight[3], const float placementUp[3], const float placementForward[3],
            float outRight[3], float outUp[3], float outForward[3])
        {
            if (baseAxis == 1)
            {
                outRight[0] = 1.0f; outRight[1] = 0.0f; outRight[2] = 0.0f;
                outUp[0] = 0.0f; outUp[1] = 0.0f; outUp[2] = 1.0f;
                outForward[0] = 0.0f; outForward[1] = -1.0f; outForward[2] = 0.0f;
                return;
            }
            if (baseAxis == 2)
            {
                const float fallbackForward[3]{ 0.0f, 0.0f, 1.0f };
                float horizontalForward[3]{ cameraForward[0], 0.0f, cameraForward[2] };
                normalize3(horizontalForward, fallbackForward);
                const float fallbackRight[3]{ horizontalForward[2], 0.0f, -horizontalForward[0] };
                float horizontalRight[3]{ cameraRight[0], 0.0f, cameraRight[2] };
                normalize3(horizontalRight, fallbackRight);
                outRight[0] = horizontalRight[0]; outRight[1] = 0.0f; outRight[2] = horizontalRight[2];
                outUp[0] = 0.0f; outUp[1] = 1.0f; outUp[2] = 0.0f;
                outForward[0] = horizontalForward[0]; outForward[1] = 0.0f; outForward[2] = horizontalForward[2];
                return;
            }
            if (baseAxis == 3)
            {
                std::copy(placementRight, placementRight + 3, outRight);
                std::copy(placementUp, placementUp + 3, outUp);
                std::copy(placementForward, placementForward + 3, outForward);
                return;
            }
            std::copy(cameraRight, cameraRight + 3, outRight);
            std::copy(cameraUp, cameraUp + 3, outUp);
            std::copy(cameraForward, cameraForward + 3, outForward);
        }

        // Rotation is defined in the object's local frame, with each axis
        // resolved through the unrotated orientation basis. Native EFT uses a
        // single authored rotation matrix: continuous rotation takes precedence
        // over the per-spawn initial rotation instead of composing both.
        // rotationSign follows the reference's usesMesh ? 1 : -1.
        void apply_particle_spin(float right[3], float up[3], float forward[3],
            const float origRight[3], const float origUp[3], const float origForward[3],
            const EftEffect& effect, float rotation, float initialRotation, float rotationSign)
        {
            float axis[3];
            if (effect.rotationEnabled && axis_vector(effect.rotationAxis, origRight, origUp, origForward, axis))
            {
                const float angle = rotationSign * rotation;
                const float cosA = std::cos(angle), sinA = std::sin(angle);
                rodrigues(right, axis, cosA, sinA);
                rodrigues(up, axis, cosA, sinA);
                rodrigues(forward, axis, cosA, sinA);
            }
            if (!effect.rotationEnabled
                && axis_vector(effect.initialRotationAxis, origRight, origUp, origForward, axis))
            {
                const float angle = rotationSign * initialRotation;
                const float cosA = std::cos(angle), sinA = std::sin(angle);
                rodrigues(right, axis, cosA, sinA);
                rodrigues(up, axis, cosA, sinA);
                rodrigues(forward, axis, cosA, sinA);
            }
        }
    }

    const EftMesh* EffectParticleSystem::mesh_for(const EftEffect& effect) const
    {
        if (effect.motionPathEnabled || effect.meshIndex < 0 || !meshes_)
            return nullptr;
        if (static_cast<std::size_t>(effect.meshIndex) >= meshes_->size())
            return nullptr;
        const auto& mesh = (*meshes_)[static_cast<std::size_t>(effect.meshIndex)];
        if (!mesh.parsed || mesh.vertices.empty() || mesh.faces.empty())
            return nullptr;
        return &mesh;
    }

    void EffectParticleSystem::build(const PhoenixRuntimeState& state)
    {
        library_ = &state.effectLibrary;
        textureLayers_ = &state.effectTextureLayers;
        meshes_ = &state.effectMeshes;
        emitters_.clear();
        // Debug-panel spawns reference indices into the library being
        // replaced above; a map (re)load invalidates them.
        debugSpawns_.clear();
        vertices_.clear();
        indices_.clear();
        instances_.clear();
        batches_.clear();

        if (!library_->parsed)
            return;

        emitters_.reserve(state.effectPlacements.size() * 2);
        for (std::size_t i = 0; i < state.effectPlacements.size(); ++i)
        {
            const auto& placement = state.effectPlacements[i];
            if (placement.sequenceIndex < 0
                || static_cast<std::size_t>(placement.sequenceIndex) >= library_->sequences.size())
                continue;

            // A world-placed "effect" is a named sequence, not a single raw
            // component: expand it into one emitter per record, each
            // starting after that record's authored delay.
            const auto& sequence = library_->sequences[static_cast<std::size_t>(placement.sequenceIndex)];
            for (std::size_t r = 0; r < sequence.records.size(); ++r)
            {
                const auto& record = sequence.records[r];
                if (record.effectId < 0 || static_cast<std::size_t>(record.effectId) >= library_->effects.size())
                    continue;

                const auto& effect = library_->effects[static_cast<std::size_t>(record.effectId)];

                Emitter emitter{};
                emitter.effectIndex = record.effectId;
                std::copy(std::begin(placement.position), std::end(placement.position), std::begin(emitter.origin));
                std::copy(std::begin(placement.right), std::end(placement.right), std::begin(emitter.right));
                std::copy(std::begin(placement.up), std::end(placement.up), std::begin(emitter.up));
                std::copy(std::begin(placement.forward), std::end(placement.forward), std::begin(emitter.forward));
                emitter.seed = static_cast<float>(i) * 977.0f + static_cast<float>(r) * 131.0f + 1.0f;
                // Negative until the record's start delay has elapsed (see step_emitter).
                emitter.elapsed = -std::max(0.0f, record.time);
                emitter.remainingDuration = effect.emitterDuration;
                emitter.particles.resize(static_cast<std::size_t>(std::min(effect_particle_count(effect), 300)));

                emitters_.push_back(std::move(emitter));
            }
        }
    }

    std::uint32_t EffectParticleSystem::spawn_debug_effect(
        const phoenix::world::EftLibrary& library,
        const std::vector<std::uint32_t>& textureLayers,
        const std::vector<phoenix::world::EftMesh>& meshes,
        std::int32_t sequenceIndex, const float position[3], float facingYawRadians, bool oneShot)
    {
        if (!library.parsed || sequenceIndex < 0
            || static_cast<std::size_t>(sequenceIndex) >= library.sequences.size())
            return 0;

        const auto& sequence = library.sequences[static_cast<std::size_t>(sequenceIndex)];

        DebugSpawn spawn{};
        spawn.id = nextDebugSpawnId_;
        spawn.library = &library;
        spawn.textureLayers = &textureLayers;
        spawn.meshes = &meshes;
        spawn.emitters.reserve(sequence.records.size());

        // Horizontal-facing placement basis (matches the camera/character yaw
        // convention: forward = (sin(yaw), 0, cos(yaw))), so baseAxis == 3
        // components face the same way the character does when spawned.
        const float facingForward[3]{ std::sin(facingYawRadians), 0.0f, std::cos(facingYawRadians) };
        const float facingUp[3]{ 0.0f, 1.0f, 0.0f };
        const float facingRight[3]{ std::cos(facingYawRadians), 0.0f, -std::sin(facingYawRadians) };

        for (std::size_t r = 0; r < sequence.records.size(); ++r)
        {
            const auto& record = sequence.records[r];
            if (record.effectId < 0 || static_cast<std::size_t>(record.effectId) >= library.effects.size())
                continue;

            const auto& effect = library.effects[static_cast<std::size_t>(record.effectId)];

            Emitter emitter{};
            emitter.effectIndex = record.effectId;
            emitter.origin[0] = position[0];
            emitter.origin[1] = position[1];
            emitter.origin[2] = position[2];
            std::copy(std::begin(facingRight), std::end(facingRight), std::begin(emitter.right));
            std::copy(std::begin(facingUp), std::end(facingUp), std::begin(emitter.up));
            std::copy(std::begin(facingForward), std::end(facingForward), std::begin(emitter.forward));
            emitter.seed = static_cast<float>(spawn.id) * 733.0f + static_cast<float>(r) * 131.0f + 1.0f;
            emitter.elapsed = -std::max(0.0f, record.time);
            emitter.remainingDuration = effect.emitterDuration;
            emitter.forceNoLoop = oneShot;

            EftEffect sizingEffect = effect;
            if (oneShot) sizingEffect.loop = false;
            emitter.particles.resize(static_cast<std::size_t>(std::min(effect_particle_count(sizingEffect), 300)));

            spawn.emitters.push_back(std::move(emitter));
        }

        if (spawn.emitters.empty())
            return 0;

        ++nextDebugSpawnId_;
        const auto id = spawn.id;
        debugSpawns_.push_back(std::move(spawn));
        return id;
    }

    std::uint32_t EffectParticleSystem::spawn_debug_component(
        const phoenix::world::EftLibrary& library,
        const std::vector<std::uint32_t>& textureLayers,
        const std::vector<phoenix::world::EftMesh>& meshes,
        std::int32_t effectIndex, const float position[3], float facingYawRadians, bool oneShot)
    {
        if (!library.parsed || effectIndex < 0
            || static_cast<std::size_t>(effectIndex) >= library.effects.size())
            return 0;

        const auto& effect = library.effects[static_cast<std::size_t>(effectIndex)];

        const float facingForward[3]{ std::sin(facingYawRadians), 0.0f, std::cos(facingYawRadians) };
        const float facingUp[3]{ 0.0f, 1.0f, 0.0f };
        const float facingRight[3]{ std::cos(facingYawRadians), 0.0f, -std::sin(facingYawRadians) };

        DebugSpawn spawn{};
        spawn.id = nextDebugSpawnId_;
        spawn.library = &library;
        spawn.textureLayers = &textureLayers;
        spawn.meshes = &meshes;

        Emitter emitter{};
        emitter.effectIndex = effectIndex;
        emitter.origin[0] = position[0];
        emitter.origin[1] = position[1];
        emitter.origin[2] = position[2];
        std::copy(std::begin(facingRight), std::end(facingRight), std::begin(emitter.right));
        std::copy(std::begin(facingUp), std::end(facingUp), std::begin(emitter.up));
        std::copy(std::begin(facingForward), std::end(facingForward), std::begin(emitter.forward));
        emitter.seed = static_cast<float>(spawn.id) * 733.0f + 1.0f;
        emitter.remainingDuration = effect.emitterDuration;
        emitter.forceNoLoop = oneShot;

        EftEffect sizingEffect = effect;
        if (oneShot) sizingEffect.loop = false;
        emitter.particles.resize(static_cast<std::size_t>(std::min(effect_particle_count(sizingEffect), 300)));

        spawn.emitters.push_back(std::move(emitter));

        ++nextDebugSpawnId_;
        const auto id = spawn.id;
        debugSpawns_.push_back(std::move(spawn));
        return id;
    }

    bool EffectParticleSystem::debug_spawn_active(std::uint32_t id) const
    {
        const auto it = std::find_if(debugSpawns_.begin(), debugSpawns_.end(),
            [id](const DebugSpawn& spawn) { return spawn.id == id; });
        if (it == debugSpawns_.end())
            return false;

        for (const auto& emitter : it->emitters)
        {
            // Two ways a (forced-non-looping, per bot spawns) emitter can
            // still have particles left to spawn: it's the single-burst
            // "one-shot" kind and hasn't fired yet, or it's the regular
            // emit-rate kind and its duration window hasn't closed yet.
            const bool willEmitMore = !emitter.oneShotEmitted && emitter.remainingDuration > 0.0f;
            if (willEmitMore)
                return true;
            for (const auto& particle : emitter.particles)
                if (particle.active)
                    return true;
        }
        return false;
    }

    void EffectParticleSystem::remove_debug_spawn(std::uint32_t id)
    {
        std::erase_if(debugSpawns_, [id](const DebugSpawn& spawn) { return spawn.id == id; });
    }

    void EffectParticleSystem::spawn_particle(Emitter& emitter, const EftEffect& effect, float life, float spawnSeconds)
    {
        const auto it = std::find_if(emitter.particles.begin(), emitter.particles.end(),
            [](const Particle& p) { return !p.active; });
        if (it == emitter.particles.end())
            return;

        const auto slotIndex = static_cast<float>(std::distance(emitter.particles.begin(), it));
        const float seed = emitter.seed + static_cast<float>(emitter.spawnCounter) * 37.17f + slotIndex * 1009.0f;
        emitter.spawnCounter += 1;

        Particle& p = *it;
        p = Particle{};
        p.active = true;
        p.life = life;
        p.seed = seed;

        const float randomAmount[3]{ random_value(seed), random_value(seed + 19.1f), random_value(seed + 47.7f) };
        for (int i = 0; i < 3; ++i)
            p.position[i] = effect.emitOrigin[i] + (randomAmount[i] * 2.0f - 1.0f) * effect.emitPositionSpread[i];

        // Motion-path effects reuse the effect's mesh as a path curve rather
        // than a shape: the particle's spawn position gets a one-time offset
        // sampled from the path's last vertex at its birth time (matches the
        // reference's createParticleState pathMesh handling — the path is
        // sampled once per particle, not re-followed over its lifetime).
        if (effect.motionPathEnabled && meshes_ && effect.meshIndex >= 0
            && static_cast<std::size_t>(effect.meshIndex) < meshes_->size())
        {
            const auto& pathMesh = (*meshes_)[static_cast<std::size_t>(effect.meshIndex)];
            if (pathMesh.parsed && !pathMesh.vertices.empty())
            {
                const auto sample = sample_mesh_frame_index(pathMesh, spawnSeconds * 30.0f);
                float pathPos[3], pathUv[2];
                sample_mesh_vertex(pathMesh, sample, pathMesh.vertices.size() - 1, pathPos, pathUv);
                for (int i = 0; i < 3; ++i) p.position[i] += pathPos[i];
            }
        }

        random_velocity(effect, seed, p.velocity);

        p.angularVelocity = effect.angularVelocityRandom
            ? random_range(std::min(0.0f, effect.angularVelocity), std::max(0.0f, effect.angularVelocity), seed + 131.1f)
            : effect.angularVelocity;
        p.initialRotation = initial_rotation(effect, seed);
    }

    void EffectParticleSystem::step_emitter(Emitter& emitter, const EftEffect& effect, float dt)
    {
        emitter.elapsed += dt;
        // Still waiting out this record's start delay (see build()).
        if (emitter.elapsed < 0.0f)
            return;

        for (auto& p : emitter.particles)
        {
            if (!p.active) continue;
            p.age += dt;
            if (p.age > p.life) { p.active = false; continue; }

            for (int axis = 0; axis < 3; ++axis)
                if (effect.velocityRandomEnabled[axis])
                    p.velocity[axis] = random_range(effect.velocityMin[axis], effect.velocityMax[axis],
                        p.seed + p.age * 997.3f + static_cast<float>(axis) * 31.7f);

            apply_velocity_mode(p.position, p.velocity, effect, p.seed, p.age, dt);

            const float velocityScale = velocity_scale_at(effect, p.age);
            if (std::abs(velocityScale) > 0.0001f)
                for (int i = 0; i < 3; ++i) p.velocity[i] += p.velocity[i] * velocityScale * dt;

            if (effect.attractEnabled)
            {
                float dir[3]{
                    effect.attractPoint[0] - p.position[0],
                    effect.attractPoint[1] - p.position[1],
                    effect.attractPoint[2] - p.position[2],
                };
                const float fallbackZero[3]{ 0.0f, 0.0f, 0.0f };
                normalize3(dir, fallbackZero);
                for (int i = 0; i < 3; ++i) p.velocity[i] += dir[i] * effect.attractStrength * dt;
            }

            if (effect.gravityEnabled)
                for (int i = 0; i < 3; ++i) p.velocity[i] += effect.acceleration[i] * dt;

            if (effect.rotationEnabled)
                p.rotation += p.angularVelocity * dt;
        }

        if (!emits_particles(effect))
        {
            if (!emitter.oneShotEmitted)
            {
                spawn_particle(emitter, effect, sampled_particle_life(effect, emitter.seed), emitter.elapsed);
                emitter.oneShotEmitted = true;
            }
            return;
        }

        const float previousDuration = emitter.remainingDuration;
        emitter.remainingDuration -= dt;
        int count = 0;

        if (is_one_shot_emitter(effect))
        {
            const bool crossedDuration = previousDuration > 0.0f && emitter.remainingDuration <= 0.0f;
            if (effect.loop || (!emitter.oneShotEmitted && (crossedDuration || effect.emitterDuration <= 0.0f)))
            {
                count = 1;
                emitter.oneShotEmitted = true;
            }
        }
        else
        {
            if (!effect.loop && previousDuration <= 0.0f)
                return;
            const float rate = random_range(effect.emitRateMin, effect.emitRateMax, emitter.seed + emitter.elapsed * 97.13f);
            const float produced = std::max(0.0f, rate) * dt + emitter.emitAccumulator;
            count = static_cast<int>(std::floor(produced));
            emitter.emitAccumulator = produced - static_cast<float>(count);
            if (!effect.loop && emitter.remainingDuration <= 0.0f)
                count = 0;
            count = std::min(count, 200);
        }

        for (int i = 0; i < count; ++i)
            spawn_particle(emitter, effect,
                sampled_particle_life(effect, emitter.seed + static_cast<float>(emitter.spawnCounter) * 37.17f), emitter.elapsed);
    }

    std::uint32_t EffectParticleSystem::select_texture_layer_for(const EftEffect& effect, float elapsedSeconds) const
    {
        if (effect.textureIds.empty() || !textureLayers_)
            return 0xFFFFFFFFu;

        std::uint32_t index = 0;
        if (effect.textureIds.size() > 1)
        {
            const float frameTime = std::max(0.033f, std::abs(effect.delayPerFrame));
            const auto frame = static_cast<std::uint32_t>(std::floor(elapsedSeconds / frameTime));
            const auto count = static_cast<std::uint32_t>(effect.textureIds.size());
            index = effect.textureLoop ? frame % count : std::min(frame, count - 1);
        }

        const auto textureId = effect.textureIds[index];
        if (textureId < 0 || static_cast<std::size_t>(textureId) >= textureLayers_->size())
            return 0xFFFFFFFFu;
        return (*textureLayers_)[static_cast<std::size_t>(textureId)];
    }

    void EffectParticleSystem::update(float deltaSeconds,
        const float cameraPosition[3],
        const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
        float cullDistance)
    {
        vertices_.clear();
        indices_.clear();
        instances_.clear();
        batches_.clear();

        if (!library_ || !library_->parsed)
            return;

        const float dt = clampf(deltaSeconds, 0.0f, 0.1f);
        const float cullDistanceSq = cullDistance * cullDistance;

        // Bucketed by (geometry, texture, blend): `mesh` is null for a
        // billboard quad, or the .3DE mesh whose real geometry this batch
        // draws — every instance in a bucket shares one small vertex/index
        // range (the mesh, or a unit quad), repeated via instancing.
        //
        // Vertex-animated meshes break that sharing (each particle can be at
        // a different point in its own animation), so those get `prebaked`
        // buckets instead: geometry is sampled and transformed to world space
        // on the CPU once per particle, drawn with a single identity instance.
        struct Bucket
        {
            const EftMesh* mesh{};
            std::uint32_t layer{};
            std::int32_t sourceBlend{};
            std::int32_t destinationBlend{};
            bool mirrorTexture{};
            std::vector<EffectParticleInstance> items;
            bool prebaked{};
            std::vector<TerrainVertex> bakedVertices;
            std::vector<std::uint32_t> bakedIndices;
        };
        std::vector<Bucket> buckets;

        const auto process_emitter = [&](Emitter& emitter) {
            if (emitter.effectIndex < 0
                || static_cast<std::size_t>(emitter.effectIndex) >= library_->effects.size())
                return;

            // Same render-distance rule as everything else (NPCs, monsters,
            // static objects capped to the active fog distance): emitters
            // beyond it don't simulate or draw at all this frame.
            const float dx = emitter.origin[0] - cameraPosition[0];
            const float dy = emitter.origin[1] - cameraPosition[1];
            const float dz = emitter.origin[2] - cameraPosition[2];
            if (dx * dx + dy * dy + dz * dz > cullDistanceSq)
                return;

            // The debug panel's "one-shot" spawns force `loop` off for this
            // instance only, regardless of how the component is authored.
            EftEffect overridden;
            const EftEffect* effectPtr = &library_->effects[static_cast<std::size_t>(emitter.effectIndex)];
            if (emitter.forceNoLoop && effectPtr->loop)
            {
                overridden = *effectPtr;
                overridden.loop = false;
                effectPtr = &overridden;
            }
            const auto& effect = *effectPtr;
            const auto* mesh = mesh_for(effect);
            // A .3DE's local draw basis rotates source X/Z by 180 degrees. Its
            // authored emitter offsets, velocities and forces live in that same
            // frame. Keep those vectors coherent with the geometry, matching
            // Godot's _effect_space_vector conversion.
            EftEffect effectSpace = effect;
            if (mesh)
            {
                for (auto* vector : { effectSpace.emitOrigin, effectSpace.emitPositionSpread,
                        effectSpace.velocityMin, effectSpace.velocityMax,
                        effectSpace.acceleration, effectSpace.attractPoint })
                {
                    vector[0] = -vector[0];
                    vector[2] = -vector[2];
                }
            }
            step_emitter(emitter, effectSpace, dt);
            if (emitter.elapsed < 0.0f)
                return; // still waiting out this record's start delay

            // Real .3DE geometry is authored in its own local space (not a
            // unit quad), so the reference renderer flips rotation sign for
            // mesh particles vs. billboards (usesMesh ? 1 : -1).
            const float rotationSign = mesh ? 1.0f : -1.0f;

            float origRight[3], origUp[3], origForward[3];
            compute_orientation_basis(effect.baseAxis, cameraRight, cameraUp, cameraForward,
                emitter.right, emitter.up, emitter.forward, origRight, origUp, origForward);

            const bool animated = mesh && !mesh->frames.empty();

            for (const auto& particle : emitter.particles)
            {
                if (!particle.active) continue;

                // Animated textures are particle-local in Godot: every birth
                // starts at frame zero instead of inheriting the emitter's
                // global phase.
                const auto textureLayer = select_texture_layer_for(effect, particle.age);
                if (textureLayer == 0xFFFFFFFFu)
                    continue;

                const Color4 color = color_at(effect, particle.age);
                const float alpha = clampf(color.a, 0.0f, 1.0f);
                if (alpha <= 0.003f) continue;

                const float scale = std::max(0.001f, scale_at(effect, particle.age, particle.seed));

                float right[3]{ origRight[0], origRight[1], origRight[2] };
                float up[3]{ origUp[0], origUp[1], origUp[2] };
                float forward[3]{ origForward[0], origForward[1], origForward[2] };
                apply_particle_spin(right, up, forward, origRight, origUp, origForward,
                    effect, particle.rotation, particle.initialRotation, rotationSign);
                // .3DE meshes are authored front-facing along -Z/-X relative to
                // the placement basis (opposite of the +Z/+X convention regular
                // SMOD entity meshes use) — flip both so a mesh-shaped effect
                // (e.g. a waterfall) faces the way it was placed instead of
                // exactly backwards. Billboards ignore right/forward's sign
                // (their local z is always 0), so this only affects real mesh
                // geometry.
                const float meshFlip = mesh ? -1.0f : 1.0f;
                for (int i = 0; i < 3; ++i) { right[i] *= scale * meshFlip; up[i] *= scale; forward[i] *= scale * meshFlip; }

                float renderPos[3];
                if (effect.velocityMode == 0)
                    for (int i = 0; i < 3; ++i) renderPos[i] = emitter.origin[i] + particle.position[i];
                else
                    for (int i = 0; i < 3; ++i) renderPos[i] = emitter.origin[i] + particle.position[i] + particle.velocity[i];

                EffectParticleInstance instance{};
                instance.color[0] = clampf(color.r, 0.0f, 4.0f);
                instance.color[1] = clampf(color.g, 0.0f, 4.0f);
                instance.color[2] = clampf(color.b, 0.0f, 4.0f);
                instance.color[3] = alpha;

                if (animated)
                {
                    // Sample this particle's own animation tick (matching the
                    // reference's textureSeconds*30 mesh-tick convention) and
                    // bake the transformed geometry straight to world space.
                    const auto sample = sample_mesh_frame_index(*mesh, particle.age * 30.0f);
                    Bucket baked{ nullptr, textureLayer, effect.sourceBlend, effect.destinationBlend, effect.mirrorTexture, {}, true, {}, {} };
                    baked.bakedVertices.reserve(mesh->vertices.size());
                    for (std::size_t v = 0; v < mesh->vertices.size(); ++v)
                    {
                        float localPos[3], localUv[2];
                        sample_mesh_vertex(*mesh, sample, v, localPos, localUv);
                        TerrainVertex vertex{};
                        for (int i = 0; i < 3; ++i)
                            vertex.position[i] = renderPos[i] + right[i] * localPos[0] + up[i] * localPos[1] + forward[i] * localPos[2];
                        vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
                        vertex.normal[0] = 0.0f; vertex.normal[1] = 0.0f; vertex.normal[2] = 1.0f;
                        vertex.uv[0] = localUv[0]; vertex.uv[1] = localUv[1];
                        vertex.textureLayer = textureLayer;
                        baked.bakedVertices.push_back(vertex);
                    }
                    baked.bakedIndices.reserve(mesh->faces.size() * 3);
                    for (const auto& face : mesh->faces)
                    {
                        baked.bakedIndices.push_back(face.indices[0]);
                        baked.bakedIndices.push_back(face.indices[1]);
                        baked.bakedIndices.push_back(face.indices[2]);
                    }
                    // Identity instance: the geometry above is already world-space.
                    instance.right[0] = 1.0f; instance.up[1] = 1.0f; instance.forward[2] = 1.0f;
                    baked.items.push_back(instance);
                    buckets.push_back(std::move(baked));
                    continue;
                }

                auto found = std::find_if(buckets.begin(), buckets.end(), [&](const Bucket& b) {
                    return !b.prebaked && b.mesh == mesh && b.layer == textureLayer
                        && b.sourceBlend == effect.sourceBlend && b.destinationBlend == effect.destinationBlend
                        && b.mirrorTexture == effect.mirrorTexture;
                });
                if (found == buckets.end())
                {
                    buckets.push_back(Bucket{ mesh, textureLayer, effect.sourceBlend, effect.destinationBlend,
                        effect.mirrorTexture, {}, false, {}, {} });
                    found = buckets.end() - 1;
                }

                instance.right[0] = right[0]; instance.right[1] = right[1]; instance.right[2] = right[2];
                instance.up[0] = up[0]; instance.up[1] = up[1]; instance.up[2] = up[2];
                instance.forward[0] = forward[0]; instance.forward[1] = forward[1]; instance.forward[2] = forward[2];
                instance.position[0] = renderPos[0]; instance.position[1] = renderPos[1]; instance.position[2] = renderPos[2];

                found->items.push_back(instance);
            }
        };

        for (auto& emitter : emitters_)
            process_emitter(emitter);

        // Debug spawns may reference an entirely different .EFT file than the
        // map's own; swap the pointers process_emitter/mesh_for/
        // select_texture_layer_for read from for the duration of that spawn.
        const auto* mapLibrary = library_;
        const auto* mapTextureLayers = textureLayers_;
        const auto* mapMeshes = meshes_;
        for (auto& spawn : debugSpawns_)
        {
            if (!spawn.library || !spawn.textureLayers || !spawn.meshes)
                continue;
            library_ = spawn.library;
            textureLayers_ = spawn.textureLayers;
            meshes_ = spawn.meshes;
            for (auto& emitter : spawn.emitters)
                process_emitter(emitter);
        }
        library_ = mapLibrary;
        textureLayers_ = mapTextureLayers;
        meshes_ = mapMeshes;

        for (const auto& bucket : buckets)
        {
            if (bucket.items.empty()) continue;

            const auto baseVertex = static_cast<std::uint32_t>(vertices_.size());
            std::uint32_t geometryIndexCount = 0;

            if (bucket.prebaked)
            {
                vertices_.insert(vertices_.end(), bucket.bakedVertices.begin(), bucket.bakedVertices.end());
                indices_.reserve(indices_.size() + bucket.bakedIndices.size());
                for (const auto localIndex : bucket.bakedIndices)
                    indices_.push_back(baseVertex + localIndex);
                geometryIndexCount = static_cast<std::uint32_t>(bucket.bakedIndices.size());
            }
            else if (bucket.mesh)
            {
                vertices_.reserve(vertices_.size() + bucket.mesh->vertices.size());
                for (const auto& meshVertex : bucket.mesh->vertices)
                {
                    TerrainVertex vertex{};
                    vertex.position[0] = meshVertex.position[0];
                    vertex.position[1] = meshVertex.position[1];
                    vertex.position[2] = meshVertex.position[2];
                    vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
                    vertex.normal[0] = 0.0f; vertex.normal[1] = 0.0f; vertex.normal[2] = 1.0f;
                    vertex.uv[0] = meshVertex.uv[0]; vertex.uv[1] = meshVertex.uv[1];
                    vertex.textureLayer = bucket.layer;
                    vertices_.push_back(vertex);
                }
                indices_.reserve(indices_.size() + bucket.mesh->faces.size() * 3);
                for (const auto& face : bucket.mesh->faces)
                {
                    indices_.push_back(baseVertex + face.indices[0]);
                    indices_.push_back(baseVertex + face.indices[1]);
                    indices_.push_back(baseVertex + face.indices[2]);
                }
                geometryIndexCount = static_cast<std::uint32_t>(bucket.mesh->faces.size() * 3);
            }
            else
            {
                if (!bucket.mirrorTexture)
                {
                    const float corners[4][2]{
                        { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f }
                    };
                    const float uvs[4][2]{
                        { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f }
                    };
                    for (int i = 0; i < 4; ++i)
                    {
                        TerrainVertex vertex{};
                        vertex.position[0] = corners[i][0];
                        vertex.position[1] = corners[i][1];
                        vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
                        vertex.normal[2] = 1.0f;
                        vertex.uv[0] = uvs[i][0]; vertex.uv[1] = uvs[i][1];
                        vertex.textureLayer = bucket.layer;
                        vertices_.push_back(vertex);
                    }
                    const std::uint32_t quadIndices[6]{ 0, 1, 2, 2, 1, 3 };
                    for (const auto quadIndex : quadIndices)
                        indices_.push_back(baseVertex + quadIndex);
                    geometryIndexCount = 6;
                }
                else
                {
                    // Godot reproduces GL_MIRRORED_REPEAT geometrically because
                    // its material cannot expose that sampler state. Use the
                    // same four half-quads here so map and Godot renderers agree
                    // even though the shared world texture sampler is GL_REPEAT.
                    for (int tileY = 0; tileY < 2; ++tileY)
                    {
                        for (int tileX = 0; tileX < 2; ++tileX)
                        {
                            const float x0 = -0.5f + static_cast<float>(tileX) * 0.5f;
                            const float y0 = -0.5f + static_cast<float>(tileY) * 0.5f;
                            const float u0 = tileX == 0 ? 1.0f : 0.0f;
                            const float u1 = tileX == 0 ? 0.0f : 1.0f;
                            const float v0 = tileY == 0 ? 0.0f : 1.0f;
                            const float v1 = tileY == 0 ? 1.0f : 0.0f;
                            const float corners[4][2]{
                                { x0, y0 }, { x0 + 0.5f, y0 },
                                { x0, y0 + 0.5f }, { x0 + 0.5f, y0 + 0.5f }
                            };
                            const float uvs[4][2]{
                                { u0, v0 }, { u1, v0 }, { u0, v1 }, { u1, v1 }
                            };
                            const auto tileBase = static_cast<std::uint32_t>(vertices_.size());
                            for (int i = 0; i < 4; ++i)
                            {
                                TerrainVertex vertex{};
                                vertex.position[0] = corners[i][0];
                                vertex.position[1] = corners[i][1];
                                vertex.color[0] = 1.0f; vertex.color[1] = 1.0f; vertex.color[2] = 1.0f;
                                vertex.normal[2] = 1.0f;
                                vertex.uv[0] = uvs[i][0]; vertex.uv[1] = uvs[i][1];
                                vertex.textureLayer = bucket.layer;
                                vertices_.push_back(vertex);
                            }
                            const std::uint32_t quadIndices[6]{ 0, 1, 2, 2, 1, 3 };
                            for (const auto quadIndex : quadIndices)
                                indices_.push_back(tileBase + quadIndex);
                        }
                    }
                    geometryIndexCount = 24;
                }
            }

            if (geometryIndexCount == 0) continue;

            const auto firstIndex = static_cast<std::uint32_t>(indices_.size()) - geometryIndexCount;
            const auto firstInstance = static_cast<std::uint32_t>(instances_.size());
            instances_.insert(instances_.end(), bucket.items.begin(), bucket.items.end());

            EffectParticleBatch batch{};
            batch.firstIndex = firstIndex;
            batch.indexCount = geometryIndexCount;
            batch.firstInstance = firstInstance;
            batch.instanceCount = static_cast<std::uint32_t>(bucket.items.size());
            batch.sourceBlend = bucket.sourceBlend;
            batch.destinationBlend = bucket.destinationBlend;
            batches_.push_back(batch);
        }
    }
}
