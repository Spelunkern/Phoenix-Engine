#pragma once

#include <filesystem>
#include <fstream>
#include <string_view>

namespace phoenix::core
{
    enum class LogOpenMode
    {
        Append,
        Truncate,
    };

    void initialize_logging(const std::filesystem::path& executableDir);
    const std::filesystem::path& logs_directory();
    const std::filesystem::path& engine_log_path();

    std::ofstream open_engine_log(LogOpenMode mode = LogOpenMode::Append);
    void write_log_line(std::string_view category, std::string_view message);
}
