#include "app/renderer_uploads.h"

#include <cstdint>

namespace phoenix::app
{
    namespace
    {
        std::vector<phoenix::renderer::TerrainVertex> character_vertices_as_terrain(
            const std::vector<phoenix::character::CharacterGpuVertex>& characterVertices)
        {
            static_assert(sizeof(phoenix::character::CharacterGpuVertex) == sizeof(phoenix::renderer::TerrainVertex),
                "CharacterGpuVertex must match TerrainVertex layout");
            const auto* terrainVerts = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(characterVertices.data());
            return { terrainVerts, terrainVerts + characterVertices.size() };
        }
    }

    void set_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible)
    {
        if (!characterSystem.ready())
            return;

        phoenix::character::PlayableInput noInput{};
        characterSystem.update(0.0f, noInput);
        auto terrainVertices = character_vertices_as_terrain(characterSystem.world_vertices());
        renderer.set_character_mesh(terrainVertices, characterSystem.indices());
        renderer.set_character_visible(visible);
    }

    void update_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible)
    {
        if (!characterSystem.ready())
            return;

        phoenix::character::PlayableInput noInput{};
        characterSystem.update(0.0f, noInput);
        auto terrainVertices = character_vertices_as_terrain(characterSystem.world_vertices());
        renderer.update_character_mesh(terrainVertices, characterSystem.indices());
        renderer.set_character_visible(visible);
    }

    void load_character_texture_slots(
        phoenix::character::CharacterSystem& characterSystem,
        std::vector<phoenix::renderer::DdsTexture>& textureSlots,
        std::size_t baseSlot,
        std::size_t slotReserve)
    {
        const auto& texturePaths = characterSystem.texture_paths();
        if (textureSlots.size() < baseSlot + slotReserve)
            textureSlots.resize(baseSlot + slotReserve);

        if (characterSystem.bc3_cache_ready())
        {
            for (std::size_t i = 0; i < texturePaths.size() && i < slotReserve; ++i)
            {
                const auto* cached = characterSystem.bc3_texture_for(texturePaths[i]);
                if (cached)
                    textureSlots[baseSlot + i] = *cached;
                else
                    textureSlots[baseSlot + i] = phoenix::renderer::load_dds(texturePaths[i]);
            }
        }
        else
        {
            for (std::size_t i = 0; i < texturePaths.size() && i < slotReserve; ++i)
                textureSlots[baseSlot + i] = phoenix::renderer::load_dds(texturePaths[i]);
        }
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(baseSlot));
    }
}
