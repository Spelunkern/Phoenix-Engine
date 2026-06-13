#pragma once

#include "character/character_system.h"
#include "renderer/vulkan_renderer.h"

#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace phoenix::character
{
    // Fully procedural particle "aura" anchored to the equipped weapon's attach
    // bone. No asset files and no effect folders. Four fixed elements (fire,
    // wind, earth, water), each a hand-tuned composition of three stacked
    // layers (core flow + detail sparks + soft glow) drawn as additive
    // billboards. Layers are internal — the only public knobs are the element
    // and the enabled flag.
    class WeaponEffect
    {
    public:
        enum class Element { Fire, Wind, Earth, Water };
        static constexpr int kElementCount = 4;
        static const char* element_name(Element e);

        WeaponEffect();

        bool& enabled() { return enabled_; }
        bool enabled() const { return enabled_; }

        Element element() const { return element_; }
        void set_element(Element e);

        // Advances all layers by dt and appends their billboards (additive)
        // to the shared per-frame particle batch. Emits nothing when disabled
        // or when no weapon is attached. `offhand` (optional, dual-wield):
        // the same particles are also emitted on the off-hand weapon.
        void update(float dt,
                    const CharacterSystem::WeaponAttachment& attach,
                    renderer::ParticleBatch& batch,
                    const CharacterSystem::WeaponAttachment* offhand = nullptr);

    private:
        static constexpr int kMaxLayers = 3;

        // One internal particle layer of an element composition.
        struct Layer
        {
            float colorStart[3]{};   // colour at birth
            float colorEnd[3]{};     // colour at death (gradient)
            float intensity{ 1.0f }; // alpha multiplier
            float spawnRate{};       // particles per second
            float flowSpeed{};       // drift along blade axis (units/s, <0 sinks)
            float lifetime{ 0.9f };  // seconds per particle
            float size{ 0.05f };     // billboard half-size at birth (world units)
            float sizeGrowth{ 1.0f };// size multiplier at death (shrink <1, grow >1)
            float flicker{};         // 0..1 cheap alpha shimmer amount
            float bladeLength{ 0.9f };// spawn spread, fraction (0..1) of the weapon's measured span
            float radius{ 0.06f };   // swirl radius around the axis
            float swirl{ 2.0f };     // tangential swirl speed (rad/s, <0 reverses)
        };

        struct Particle
        {
            float along{};
            float angle{};
            float seedRadius{};
            float ageS{};
            float lifeS{};
            bool alive{};
        };

        struct LayerRuntime
        {
            std::vector<Particle> particles;
            float spawnAccumulator{ 0.0f };
        };

        void spawn(const Layer& layer, Particle& p);
        void simulate_layer(float dt, const Layer& layer, LayerRuntime& rt,
                            const CharacterSystem::WeaponAttachment& attach,
                            const CharacterSystem::WeaponAttachment* offhand,
                            renderer::ParticleBatch& batch);

        bool enabled_{ false };
        Element element_{ Element::Fire };
        std::array<Layer, kMaxLayers> layers_{};
        std::array<LayerRuntime, kMaxLayers> runtime_{};
        std::mt19937 rng_{ 0x5EED1234u };
    };
}
