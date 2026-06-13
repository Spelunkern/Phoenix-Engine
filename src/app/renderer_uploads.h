#pragma once

#include "character/character_system.h"
#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"

#include <cstddef>
#include <vector>

namespace phoenix::app
{
    // shadowVisible: append a cheap blob shadow — the character silhouette
    // flattened onto the ground plane as flat dark blended triangles. Playable
    // character only (bots render through a different pipeline).
    void set_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible,
        bool shadowVisible);

    void update_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible,
        bool shadowVisible);

    // Per-frame vertex refresh (no index changes). When the shadow is enabled,
    // the flattened copies are rebuilt behind the current pose into `scratch`
    // (caller-owned to avoid a per-frame allocation).
    void update_character_vertices(
        phoenix::renderer::VulkanRenderer& renderer,
        const phoenix::character::CharacterSystem& characterSystem,
        std::vector<phoenix::renderer::TerrainVertex>& scratch,
        bool shadowVisible);

    void load_character_texture_slots(
        phoenix::character::CharacterSystem& characterSystem,
        std::vector<phoenix::renderer::DdsTexture>& textureSlots,
        std::size_t baseSlot,
        std::size_t slotReserve);
}
