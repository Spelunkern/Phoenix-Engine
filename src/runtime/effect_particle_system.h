#pragma once

#include "renderer/opengl_renderer.h"
#include "world/eft_loader.h"
#include "world/eft_mesh_loader.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace phoenix::runtime
{
    struct EffectPlacement;
    struct PhoenixRuntimeState;

    // CPU particle simulation for map-placed .EFT effects (WldAnalysis::
    // effectInstances). Owns one emitter per placement, steps them every frame,
    // and produces ready-to-upload geometry grouped into batches by
    // (mesh, texture, blend mode) — see effect_particle.vert/.frag and
    // OpenGLRenderer::update_effect_particles().
    //
    // Simulation semantics (velocity modes, gravity/attract, color/opacity/
    // scale keyframes, texture-frame animation, blend factors) follow the
    // Godot game's established map-effect path. Components with a valid,
    // non-motion-path mesh (EftEffect::meshIndex) render and animate their
    // actual .3DE geometry; everything else renders as a camera/placement-
    // oriented billboard quad.
    class EffectParticleSystem
    {
    public:
        // Drops map-owned emitters, particles and CPU draw buffers before a
        // replacement map starts loading.
        void release_map_resources();

        // (Re)builds emitter placements for the currently loaded map. Safe to
        // call with an empty/unparsed library (clears to no-op state).
        void build(const PhoenixRuntimeState& state);

        // Advances every emitter by deltaSeconds and refreshes the draw
        // buffers. cameraRight/Up/Forward are the camera's world-space basis
        // (right-handed, forward = view direction) used for camera-facing
        // (baseAxis == 0) and horizon-locked (baseAxis == 2) billboards.
        // Emitters farther than cullDistance from cameraPosition are skipped
        // entirely (no simulation, no draw) — the same render-distance rule
        // applied to NPCs/monsters (capped to the active fog distance), so
        // effects don't keep simulating indefinitely far outside view.
        void update(float deltaSeconds,
            const float cameraPosition[3],
            const float cameraRight[3], const float cameraUp[3], const float cameraForward[3],
            float cullDistance);

        bool has_particles() const { return !batches_.empty(); }
        const std::vector<phoenix::renderer::TerrainVertex>& vertices() const { return vertices_; }
        const std::vector<std::uint32_t>& indices() const { return indices_; }
        const std::vector<phoenix::renderer::EffectParticleInstance>& instances() const { return instances_; }
        const std::vector<phoenix::renderer::EffectParticleBatch>& batches() const { return batches_; }

        // Manual invocation for the "Effects" debug panel — independent of the
        // map's authored placements, at an arbitrary world position, facing
        // `facingYawRadians` (0 = +Z, matching the camera/character yaw
        // convention elsewhere — see main.cpp's cameraForwardWorld). Only
        // affects baseAxis == 3 ("uses placement basis") components; camera-
        // facing billboards ignore it entirely.
        // `library`/`textureLayers`/`meshes` may be an entirely different
        // .EFT file than the map's own (e.g. any file from PhoenixRuntime::
        // effect_library_files(), loaded via load_effect_library_file()) — the
        // caller owns that data and must keep it alive for as long as this
        // spawn exists (until clear_debug_effects() or the next build()).
        // `sequenceIndex` indexes `library.sequences`. oneShot=true forces
        // every component's `loop` off for this spawn (regardless of how it's
        // authored), so it plays through once instead of running forever;
        // oneShot=false lets each component behave exactly as it would if
        // authored on the map (some sub-components may still loop forever,
        // matching real placements).
        // Returns a nonzero spawn id, or 0 if the sequence index is invalid.
        std::uint32_t spawn_debug_effect(
            const phoenix::world::EftLibrary& library,
            const std::vector<std::uint32_t>& textureLayers,
            const std::vector<phoenix::world::EftMesh>& meshes,
            std::int32_t sequenceIndex, const float position[3], float facingYawRadians, bool oneShot);
        void clear_debug_effects() { debugSpawns_.clear(); }
        std::size_t debug_effect_count() const { return debugSpawns_.size(); }

        // True if `id` still has anything left to do — an emitter that
        // hasn't fired its one-shot burst yet, or any particle still alive.
        // False once it's finished playing out on its own, or if `id` isn't
        // a currently-tracked spawn at all (already removed). Lets a caller
        // (e.g. bots — see BotManager::onCastEffect) poll for "is this one
        // done yet" instead of guessing a fixed lifetime.
        bool debug_spawn_active(std::uint32_t id) const;
        // Force-removes one spawn by id, regardless of whether it's finished
        // — a safety backstop for callers that can't rely on it always
        // cleaning itself up (e.g. a malformed/looping component).
        void remove_debug_spawn(std::uint32_t id);

        // Diagnostic-isolation variant of spawn_debug_effect: spawns exactly
        // one raw component (an index into library.effects directly, not a
        // sequence), skipping sequence expansion entirely. Useful for
        // narrowing which specific component of a multi-part sequence is
        // misbehaving.
        std::uint32_t spawn_debug_component(
            const phoenix::world::EftLibrary& library,
            const std::vector<std::uint32_t>& textureLayers,
            const std::vector<phoenix::world::EftMesh>& meshes,
            std::int32_t effectIndex, const float position[3], float facingYawRadians, bool oneShot);

    private:
        struct Particle
        {
            bool active{};
            float age{};
            float life{};
            float position[3]{};
            float velocity[3]{};
            float rotation{};
            float angularVelocity{};
            float initialRotation{};
            float seed{};
        };

        struct Emitter
        {
            std::int32_t effectIndex{ -1 };
            float origin[3]{};
            float right[3]{};
            float up[3]{};
            float forward[3]{};
            float elapsed{};
            float remainingDuration{};
            float emitAccumulator{};
            float seed{};
            bool oneShotEmitted{};
            // World-map emitters use the same shared visual clock, mesh-frame
            // convention and blend-mode approximation as Godot. Debug/gameplay
            // spawns retain the native inclusive-frame and exact-blend path.
            bool ambientMap{};
            // Debug-panel override: forces this emitter's effect to behave as
            // non-looping regardless of its authored `loop` flag.
            bool forceNoLoop{};
            std::size_t placementIndex{ std::numeric_limits<std::size_t>::max() };
            std::uint32_t spawnCounter{};
            std::vector<Particle> particles;
        };

        // One "Spawn" click from the debug panel: one emitter per record of
        // the chosen sequence, all sharing a position/id so they can be
        // counted and cleared together.
        struct DebugSpawn
        {
            std::uint32_t id{};
            // Caller-owned (see spawn_debug_effect); may differ from the map's
            // own library_/textureLayers_/meshes_.
            const phoenix::world::EftLibrary* library{};
            const std::vector<std::uint32_t>* textureLayers{};
            const std::vector<phoenix::world::EftMesh>* meshes{};
            std::vector<Emitter> emitters;
        };

        void step_emitter(Emitter& emitter, const phoenix::world::EftEffect& effect, float dt);
        void activate_map_placement(std::size_t placementIndex);
        void spawn_particle(Emitter& emitter, const phoenix::world::EftEffect& effect, float life, float spawnSeconds);
        std::uint32_t select_texture_layer_for(const phoenix::world::EftEffect& effect, float elapsedSeconds) const;
        // The mesh to use for this effect, or nullptr for a billboard quad
        // (no mesh assigned, mesh failed to load, or motion-path mode).
        const phoenix::world::EftMesh* mesh_for(const phoenix::world::EftEffect& effect) const;

        const phoenix::world::EftLibrary* library_{};
        const std::vector<std::uint32_t>* textureLayers_{};
        const std::vector<phoenix::world::EftMesh>* meshes_{};
        const std::vector<EffectPlacement>* placements_{};
        std::vector<std::uint8_t> activePlacements_;
        std::vector<Emitter> emitters_;
        std::vector<DebugSpawn> debugSpawns_;
        std::uint32_t nextDebugSpawnId_{ 1 };

        std::vector<phoenix::renderer::TerrainVertex> vertices_;
        std::vector<std::uint32_t> indices_;
        std::vector<phoenix::renderer::EffectParticleInstance> instances_;
        std::vector<phoenix::renderer::EffectParticleBatch> batches_;
    };
}
