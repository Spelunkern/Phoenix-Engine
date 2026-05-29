#include "assets/data_index.h"

#include <algorithm>
#include <cctype>

namespace phoenix::assets
{
    std::string lower_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    std::string trim_ascii(std::string value)
    {
        auto isTrim = [](unsigned char ch) {
            return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
        };
        std::size_t begin = 0;
        std::size_t end = value.size();
        while (begin < end && isTrim(static_cast<unsigned char>(value[begin])))
            ++begin;
        while (end > begin && isTrim(static_cast<unsigned char>(value[end - 1])))
            --end;
        return value.substr(begin, end - begin);
    }

    std::filesystem::path resolve_existing_path_case_insensitive(const std::filesystem::path& requested)
    {
        if (requested.empty())
            return {};

        std::error_code ec;
        if (std::filesystem::exists(requested, ec))
            return requested;

        // Walk the path component by component, fixing the case of each segment
        // against what actually exists on disk.
        std::filesystem::path result;
        bool first = true;
        for (const auto& comp : requested)
        {
            if (first)
            {
                result = comp;   // root ("/" or drive) or first relative segment
                first = false;
                continue;
            }

            std::filesystem::path candidate = result / comp;
            if (std::filesystem::exists(candidate, ec))
            {
                result = std::move(candidate);
                continue;
            }

            if (!std::filesystem::is_directory(result, ec))
                return {};

            const auto wanted = lower_ascii(comp.string());
            bool found = false;
            for (const auto& entry : std::filesystem::directory_iterator(result, ec))
            {
                if (lower_ascii(entry.path().filename().string()) == wanted)
                {
                    result = entry.path();
                    found = true;
                    break;
                }
            }
            if (!found)
                return {};
        }

        return std::filesystem::exists(result, ec) ? result : std::filesystem::path{};
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

        const auto requested = std::filesystem::path(std::string(assetName));
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

        const auto requested = std::filesystem::path(std::string(assetName));
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
