#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace phoenix::core
{
    enum class LogOpenMode
    {
        Append,
        Truncate,
    };

    enum class LogCategory
    {
        Engine,     // Lifecycle: init, shutdown, fatal errors
        Renderer,   // Pipeline creation, shader loads, swapchain, GPU features
        Assets,     // Texture/model/animation loading, cache, timings
        World,      // Map loading, terrain, actor grid, portals, objects
        Audio,      // Audio device, tracks, emitters
        Session,    // Hardware info, OS, working dir — context for debugging
        Errors,     // Critical failures, missing files, broken state — across all systems
    };

    void initialize_logging(const std::filesystem::path& executableDir);
    const std::filesystem::path& logs_directory();
    const std::filesystem::path& engine_log_path();
    std::filesystem::path specialized_log_path(std::string_view name);
    std::filesystem::path category_log_path(LogCategory category);

    std::ofstream open_engine_log(LogOpenMode mode = LogOpenMode::Append);
    std::ofstream open_log(LogCategory category, LogOpenMode mode = LogOpenMode::Append);

    void write_log_line(std::string_view category, std::string_view message);
    void log(LogCategory category, std::string_view message);
}
