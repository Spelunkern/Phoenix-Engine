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

        // Cheap blob shadow: duplicate the already-skinned character triangles
        // flattened onto the ground plane, using the shader's shadow path
        // (textureLayer 0xFFFFFFFC -> flat black, alpha taken from color.r,
        // blended). Coplanar duplicates resolve via depth-equal reject, so
        // overlap doesn't double-darken.
        void append_character_shadow_vertices(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            float groundY)
        {
            const auto baseVertex = vertices.size();
            const float shadowY = groundY + 0.02f;  // hover above terrain to avoid z-fighting
            vertices.reserve(baseVertex * 2u);
            for (std::size_t i = 0; i < baseVertex; ++i)
            {
                auto v = vertices[i];
                v.position[1] = shadowY;
                // Shadow sentinel layer: flat black, opacity in color.r.
                v.color[0] = 0.35f; v.color[1] = 0.0f; v.color[2] = 0.0f;
                v.normal[0] = 0.0f; v.normal[1] = 1.0f; v.normal[2] = 0.0f;
                v.textureLayer = 0xFFFFFFFCu;
                vertices.push_back(v);
            }
        }

        void append_character_shadow(
            std::vector<phoenix::renderer::TerrainVertex>& vertices,
            std::vector<std::uint32_t>& indices,
            float groundY)
        {
            const auto baseVertex = static_cast<std::uint32_t>(vertices.size());
            const auto baseIndexCount = indices.size();
            append_character_shadow_vertices(vertices, groundY);
            indices.reserve(baseIndexCount * 2u);
            for (std::size_t i = 0; i < baseIndexCount; ++i)
                indices.push_back(indices[i] + baseVertex);
        }
    }

    void set_character_mesh(
        phoenix::renderer::OpenGLRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible,
        bool shadowVisible)
    {
        if (!characterSystem.ready())
            return;

        phoenix::character::PlayableInput noInput{};
        characterSystem.update(0.0f, noInput);
        auto terrainVertices = character_vertices_as_terrain(characterSystem.world_vertices());
        if (shadowVisible)
        {
            auto indices = characterSystem.indices();
            append_character_shadow(terrainVertices, indices, characterSystem.render_ground_y());
            renderer.set_character_mesh(terrainVertices, indices);
        }
        else
        {
            renderer.set_character_mesh(terrainVertices, characterSystem.indices());
        }
        renderer.set_character_visible(visible);
    }

    void update_character_mesh(
        phoenix::renderer::OpenGLRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible,
        bool shadowVisible)
    {
        if (!characterSystem.ready())
            return;

        phoenix::character::PlayableInput noInput{};
        characterSystem.update(0.0f, noInput);
        auto terrainVertices = character_vertices_as_terrain(characterSystem.world_vertices());
        if (shadowVisible)
        {
            auto indices = characterSystem.indices();
            append_character_shadow(terrainVertices, indices, characterSystem.render_ground_y());
            renderer.update_character_mesh(terrainVertices, indices);
        }
        else
        {
            renderer.update_character_mesh(terrainVertices, characterSystem.indices());
        }
        renderer.set_character_visible(visible);
    }

    void update_character_vertices(
        phoenix::renderer::OpenGLRenderer& renderer,
        const phoenix::character::CharacterSystem& characterSystem,
        std::vector<phoenix::renderer::TerrainVertex>& scratch,
        bool shadowVisible)
    {
        const auto& charVerts = characterSystem.world_vertices();
        static_assert(sizeof(phoenix::character::CharacterGpuVertex) == sizeof(phoenix::renderer::TerrainVertex),
            "CharacterGpuVertex must match TerrainVertex layout");
        const auto* tv = reinterpret_cast<const phoenix::renderer::TerrainVertex*>(charVerts.data());
        if (!shadowVisible)
        {
            renderer.update_character_vertices(tv, charVerts.size());
            return;
        }
        // The shadow range lives past the character vertices in the same GPU
        // buffer (laid out by set_character_mesh), so the per-frame upload must
        // re-append the flattened copies behind the current pose.
        scratch.assign(tv, tv + charVerts.size());
        append_character_shadow_vertices(scratch, characterSystem.render_ground_y());
        renderer.update_character_vertices(scratch.data(), scratch.size());
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

        for (std::size_t i = 0; i < texturePaths.size() && i < slotReserve; ++i)
            textureSlots[baseSlot + i] = phoenix::renderer::load_dds(texturePaths[i]);
        characterSystem.set_texture_layer_base(static_cast<std::uint32_t>(baseSlot));
    }
}
