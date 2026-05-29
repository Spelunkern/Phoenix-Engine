#pragma once

#include "renderer/vulkan_renderer.h"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace phoenix::effects
{
    // How a layer's particles are initially distributed/launched.
    enum class EmitterShape
    {
        Point,   // all from the origin, random directions (sparks/impact)
        Sphere,  // filled sphere, launched outward (burst)
        Ring,    // on a circle in the local XZ plane (portal rim)
        Disc,    // filled disc in the local XZ plane (ground glow / cast)
        Cone,    // upward cone of directions (spray / fountain)
        Line,    // along the local Y axis (beam / pillar)
        Shockwave, // on a small ring, launched radially outward in the XZ plane
    };

    enum class Blend { Additive, Alpha };

    // Elemental / thematic family an effect belongs to (for spell kits, map props,
    // and UI grouping).
    enum class EffectCategory
    {
        Normal, Fire, Water, Ice, Wind, Earth, Rock,
        Lightning, Holy, Shadow, Nature, Arcane, Poison,
        Count
    };
    const char* category_name(EffectCategory category);

    // One particle layer of an effect (purely procedural billboards, no textures).
    struct EffectLayer
    {
        EmitterShape shape{ EmitterShape::Sphere };
        Blend blend{ Blend::Additive };
        float colorStart[3]{ 1.0f, 0.6f, 0.2f };  // colour at birth
        float colorEnd[3]{ 0.6f, 0.1f, 0.0f };    // colour at death
        float intensity{ 1.0f };                  // alpha multiplier
        float spawnRate{ 80.0f };                 // particles per second
        float lifetime{ 0.8f };                   // seconds per particle
        float size{ 0.15f };                      // billboard half-size (world units)
        float radius{ 0.5f };                     // shape radius
        float height{ 1.0f };                     // shape height (cone/line)
        float speed{ 0.8f };                      // initial launch speed
        float gravity{ 0.0f };                    // world-down accel (neg = rises)
        float drag{ 0.0f };                       // velocity damping (per second)
        float coneAngleDeg{ 25.0f };              // half-angle for Cone
        float originHeight{ 0.0f };               // spawn offset up the local Y axis
                                                  // (e.g. rain / rocks falling from the sky)
        float swirl{ 0.0f };                      // tangential orbit speed around the
                                                  // anchor's Y axis (tornadoes/vortices)
        bool enabled{ true };
    };

    static constexpr int kMaxEffectLayers = 3;

    // Reusable, named effect description. loop=true for auras/portals/map props;
    // loop=false for one-shots (impacts), which emit for `duration` then fade out.
    struct EffectDefinition
    {
        std::string name;
        EffectCategory category{ EffectCategory::Normal };
        std::array<EffectLayer, kMaxEffectLayers> layers{};
        int layerCount{ 1 };
        bool loop{ true };
        float duration{ 0.6f };
        // Projectiles travel forward from the spawn point. When projectile=true the
        // UI launches the effect in the facing direction; the instance moves at
        // projectileSpeed for projectileRange units, trailing its particles.
        bool projectile{ false };
        float projectileSpeed{ 0.0f };
        float projectileRange{ 0.0f };
        float projectileGravity{ 0.0f };  // >0 makes the projectile arc downward
    };

    // World placement. basis columns map local X/Y/Z to world (identity * scale for
    // world-static; pass an entity/bone basis to attach to something that moves).
    struct EffectAnchor
    {
        float position[3]{ 0.0f, 0.0f, 0.0f };
        float basis[9]{ 1, 0, 0, 0, 1, 0, 0, 0, 1 };

        static EffectAnchor at(float x, float y, float z, float scale = 1.0f)
        {
            EffectAnchor a;
            a.position[0] = x; a.position[1] = y; a.position[2] = z;
            a.basis[0] = scale; a.basis[4] = scale; a.basis[8] = scale;
            return a;
        }
    };

    // Owns and simulates all non-weapon scene effects. Particles are integrated in
    // world space (spawned relative to the anchor, then independent), so attached
    // effects trail naturally as their anchor moves.
    class EffectManager
    {
    public:
        using Handle = std::uint32_t;
        static constexpr Handle kInvalid = 0;

        // Spawn an effect instance. Keep the handle for looping effects you want to
        // later move (set_anchor) or stop. One-shots despawn themselves. Pass a
        // velocity + travelTime to launch a moving projectile (the anchor advances
        // each frame and stops emitting after travelTime so the trail dissipates).
        Handle spawn(const EffectDefinition& def, const EffectAnchor& anchor,
                     const float velocity[3] = nullptr, float travelTime = 0.0f);
        void set_anchor(Handle handle, const EffectAnchor& anchor);
        void stop(Handle handle);   // stop emitting; existing particles fade out
        void clear();

        // Advance all instances and append their billboards to the shared batch.
        void update(float dt, renderer::ParticleBatch& batch);

        std::size_t active_count() const { return instances_.size(); }

    private:
        struct Particle
        {
            float pos[3]{};
            float vel[3]{};
            float ageS{};
            float lifeS{};
            bool alive{};
        };
        struct LayerRuntime
        {
            std::vector<Particle> particles;
            float spawnAccumulator{ 0.0f };
        };
        struct Instance
        {
            Handle id{};
            EffectDefinition def;
            EffectAnchor anchor;
            std::array<LayerRuntime, kMaxEffectLayers> runtime{};
            float ageS{ 0.0f };
            bool emitting{ true };
            bool moving{ false };
            float velocity[3]{ 0.0f, 0.0f, 0.0f };
            float travelTime{ 0.0f };
        };

        void spawn_particle(const EffectLayer& layer, const EffectAnchor& anchor, Particle& out);
        Instance* find(Handle handle);

        std::vector<Instance> instances_;
        Handle nextHandle_{ 1 };
        std::mt19937 rng_{ 0xEFFEC75u };
    };

    // ---- Built-in preset catalog (code-defined definitions) ----
    // The full library of effects, grouped by category. Each EffectDefinition
    // carries its own name + category, so the UI can list and filter them.
    // Spell-oriented one-shots and looping map props live side by side.
    const std::vector<EffectDefinition>& preset_catalog();

    // Convenience used by the demo attack key (G).
    EffectDefinition preset_impact();
}
