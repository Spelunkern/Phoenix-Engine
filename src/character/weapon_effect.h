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
    // bone. No asset files and no effect folders. Supports several stacked layers
    // (e.g. a bright core + a slow halo), each with its own birth->death colour
    // gradient, so colours and effects can be freely combined. Particles flow
    // along the weapon's blade axis with a swirl and are drawn as soft additive
    // billboards. Every parameter is live-tunable from ImGui.
    class WeaponEffect
    {
    public:
        static constexpr int kMaxLayers = 3;

        // One independent particle layer.
        struct Layer
        {
            bool enabled{ false };
            float colorStart[3]{ 0.35f, 0.65f, 1.0f }; // colour at birth
            float colorEnd[3]{ 0.10f, 0.20f, 0.6f };   // colour at death (gradient)
            float intensity{ 1.0f };                   // alpha multiplier
            float spawnRate{ 90.0f };                  // particles per second
            float flowSpeed{ 0.7f };                   // drift along blade axis (units/s)
            float lifetime{ 0.9f };                    // seconds per particle
            float size{ 0.05f };                       // billboard half-size (world units)
            float bladeLength{ 0.9f };                 // spread along the blade axis
            float radius{ 0.06f };                     // swirl radius around the axis
            float swirl{ 2.0f };                       // tangential swirl speed (rad/s)
            int axis{ 1 };                             // blade axis (0=X,1=Y,2=Z)
        };

        // Element presets that fill a layer's colours + tuned parameters.
        enum class Preset { Fire, Ice, Holy, Poison, Shadow, Arcane };
        static constexpr int kPresetCount = 6;
        static const char* preset_name(Preset p);

        WeaponEffect();

        bool& enabled() { return enabled_; }
        bool enabled() const { return enabled_; }
        Layer& layer(int i) { return layers_[static_cast<std::size_t>(i)]; }
        const Layer& layer(int i) const { return layers_[static_cast<std::size_t>(i)]; }

        // Overwrite one layer with a preset (and enable it).
        void apply_preset(int layerIndex, Preset preset);

        // Advances all enabled layers by dt and pushes the combined billboard list
        // to the renderer. Clears the renderer's particle list once when fully
        // disabled or when no weapon is attached.
        void update(float dt,
                    const CharacterSystem::WeaponAttachment& attach,
                    renderer::VulkanRenderer& renderer);

    private:
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
                            const CharacterSystem::WeaponAttachment& attach);

        bool enabled_{ false };
        std::array<Layer, kMaxLayers> layers_{};
        std::array<LayerRuntime, kMaxLayers> runtime_{};
        bool clearedOnce_{ true };
        std::mt19937 rng_{ 0x5EED1234u };
        std::vector<renderer::ParticleInstance> scratch_;
    };
}
