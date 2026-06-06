#pragma once

#include "character/character_system.h"
#include "renderer/dds_loader.h"
#include "renderer/vulkan_renderer.h"

#include <cstddef>
#include <vector>

namespace phoenix::app
{
    void set_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible);

    void update_character_mesh(
        phoenix::renderer::VulkanRenderer& renderer,
        phoenix::character::CharacterSystem& characterSystem,
        bool visible);

    void load_character_texture_slots(
        phoenix::character::CharacterSystem& characterSystem,
        std::vector<phoenix::renderer::DdsTexture>& textureSlots,
        std::size_t baseSlot,
        std::size_t slotReserve);
}
