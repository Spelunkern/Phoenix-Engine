#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phoenix::assets
{
    std::string lower_ascii(std::string value);

    // Strip leading/trailing ASCII whitespace, including the trailing '\r' left
    // behind when a Windows CRLF file is read on Linux. Used by CSV parsers so a
    // last-column value like "humf_torso0072.dds\r" resolves to a real file.
    std::string trim_ascii(std::string value);

    // Resolve a path that may differ from disk only in letter case (Linux is
    // case-sensitive; Windows is not). Returns the existing path if found,
    // walking each component case-insensitively from an existing base; returns an
    // empty path when no match exists.
    std::filesystem::path resolve_existing_path_case_insensitive(const std::filesystem::path& requested);

    struct DataIndex
    {
        std::filesystem::path root;
        std::uint32_t indexedFiles{};
        std::unordered_map<std::string, std::filesystem::path> byRelativePath;
        std::unordered_map<std::string, std::filesystem::path> byFileName;

        static std::string normalize_key(std::filesystem::path path);

        std::filesystem::path resolve(std::string_view assetName) const;
        std::filesystem::path resolve_relative(std::string_view assetName) const;
    };

    DataIndex index_data_directory(const std::filesystem::path& dataRoot);
    std::filesystem::path resolve_texture_asset(const DataIndex& assets, std::string textureName);

    // Case-insensitive file I/O: try the path as-is first (fast on Windows/correct
    // case), then fall back to resolve_existing_path_case_insensitive on failure.
    std::ifstream open_ifstream(const std::filesystem::path& path,
                                std::ios::openmode mode = std::ios::in);
    std::vector<std::uint8_t> read_file_binary(const std::filesystem::path& path);
}
