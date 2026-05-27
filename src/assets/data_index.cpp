#include "assets/data_index.h"

#include <algorithm>
#include <cctype>

namespace phoenix::assets
{
    std::wstring widen_ascii(std::string_view value)
    {
        return { value.begin(), value.end() };
    }

    std::string narrow_ascii(std::wstring_view value)
    {
        std::string result;
        result.reserve(value.size());
        for (const auto ch : value)
            result.push_back(ch <= 0x7F ? static_cast<char>(ch) : '?');
        return result;
    }

    std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string DataIndex::normalize_key(std::filesystem::path path)
    {
        auto value = path.generic_string();
        std::replace(value.begin(), value.end(), '/', '\\');
        return lower_ascii(std::move(value));
    }

    std::filesystem::path DataIndex::resolve(std::string_view assetName) const
    {
        if (assetName.empty())
            return {};

        const auto requested = std::filesystem::path(widen_ascii(assetName));
        if (requested.is_absolute() && std::filesystem::exists(requested))
            return requested;

        const auto direct = root / requested;
        if (std::filesystem::exists(direct))
            return direct;

        const auto key = normalize_key(requested);
        if (const auto it = byRelativePath.find(key); it != byRelativePath.end())
            return it->second;

        if (const auto fileIt = byFileName.find(lower_ascii(requested.filename().string())); fileIt != byFileName.end())
            return fileIt->second;

        for (const auto& [relativePath, path] : byRelativePath)
        {
            if (relativePath.ends_with("\\" + key))
                return path;
        }

        return {};
    }

    std::filesystem::path DataIndex::resolve_relative(std::string_view assetName) const
    {
        if (assetName.empty())
            return {};

        const auto requested = std::filesystem::path(widen_ascii(assetName));
        const auto direct = root / requested;
        if (std::filesystem::exists(direct))
            return direct;

        if (const auto it = byRelativePath.find(normalize_key(requested)); it != byRelativePath.end())
            return it->second;

        return {};
    }

    DataIndex index_data_directory(const std::filesystem::path& dataRoot)
    {
        DataIndex index{};
        index.root = dataRoot;
        if (!std::filesystem::exists(dataRoot))
            return index;

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dataRoot))
        {
            if (!entry.is_regular_file())
                continue;

            const auto path = entry.path();
            const auto relativePath = std::filesystem::relative(path, dataRoot);
            index.byRelativePath.try_emplace(DataIndex::normalize_key(relativePath), path);
            index.byFileName.try_emplace(lower_ascii(path.filename().string()), path);
            ++index.indexedFiles;
        }

        return index;
    }

    std::filesystem::path resolve_texture_asset(const DataIndex& assets, std::string textureName)
    {
        auto path = assets.resolve(textureName);
        if (!path.empty())
            return path;

        const auto ext = lower_ascii(std::filesystem::path(textureName).extension().string());
        if (ext == ".tga")
        {
            textureName.replace(textureName.size() - 4, 4, ".dds");
            path = assets.resolve(textureName);
            if (!path.empty())
                return path;
            textureName.replace(textureName.size() - 4, 4, ".bmp");
            return assets.resolve(textureName);
        }
        if (ext == ".bmp")
        {
            textureName.replace(textureName.size() - 4, 4, ".dds");
            path = assets.resolve(textureName);
            if (!path.empty())
                return path;
            textureName.replace(textureName.size() - 4, 4, ".tga");
            return assets.resolve(textureName);
        }
        if (ext == ".dds")
        {
            textureName.replace(textureName.size() - 4, 4, ".tga");
            path = assets.resolve(textureName);
            if (!path.empty())
                return path;
            textureName.replace(textureName.size() - 4, 4, ".bmp");
            return assets.resolve(textureName);
        }

        return {};
    }
}
