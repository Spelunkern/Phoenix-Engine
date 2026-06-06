#pragma once

#include "renderer/vulkan_renderer.h"

#include <filesystem>

namespace phoenix::app
{
    std::filesystem::path executable_directory();
    void release_memory_to_os();
    void truncate_periodic_diagnostic_logs();
    void write_startup_session_log(const std::filesystem::path& executableDir);
    void write_renderer_session_log(const phoenix::renderer::VulkanRenderer& renderer);
}
