#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

namespace phoenix::assets
{
    std::string lower_ascii(std::string value);

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
}
