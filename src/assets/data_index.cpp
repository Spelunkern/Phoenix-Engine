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

        // Normalize backslash separators from Windows-style asset references.
        std::string normalizedName(assetName);
        std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');
        const auto requested = std::filesystem::path(normalizedName);
        if (requested.is_absolute() && std::filesystem::exists(requested))
            return requested;

        const auto direct = root / requested;
        if (std::filesystem::exists(direct))
            return direct;

        // Case-insensitive fallback for Linux.
        const auto ciDirect = resolve_existing_path_case_insensitive(direct);
        if (!ciDirect.empty())
            return ciDirect;

        const auto key = normalize_key(requested);
        if (const auto it = byRelativePath.find(key); it != byRelativePath.end())
            return it->second;

        if (const auto fileIt = byFileName.find(lower_ascii(requested.filename().string())); fileIt != byFileName.end())
            return fileIt->second;

        const auto suffix = "\\" + key;   // hoisted out of the loop (was allocating per iteration)
        for (const auto& [relativePath, path] : byRelativePath)
        {
            if (relativePath.ends_with(suffix))
                return path;
        }

        return {};
    }

    std::filesystem::path DataIndex::resolve_relative(std::string_view assetName) const
    {
        if (assetName.empty())
            return {};

        std::string normalizedName(assetName);
        std::replace(normalizedName.begin(), normalizedName.end(), '\\', '/');
        const auto requested = std::filesystem::path(normalizedName);
        const auto direct = root / requested;
        if (std::filesystem::exists(direct))
            return direct;

        const auto ciDirect = resolve_existing_path_case_insensitive(direct);
        if (!ciDirect.empty())
            return ciDirect;

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

        // Reserve up front to avoid repeated rehashing while inserting ~20k+ files.
        index.byRelativePath.reserve(32768);
        index.byFileName.reserve(32768);

        std::error_code ec;
        std::filesystem::recursive_directory_iterator it(
            dataRoot, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec))
        {
            if (ec)
                break;
            const auto& entry = *it;
            if (!entry.is_regular_file(ec))
                continue;

            const auto& path = entry.path();
            // lexically_relative is pure string math (no filesystem access), unlike
            // std::filesystem::relative which can stat/resolve — a big cost over 20k+ files.
            const auto relativePath = path.lexically_relative(dataRoot);
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

    std::ifstream open_ifstream(const std::filesystem::path& path, std::ios::openmode mode)
    {
        std::ifstream stream(path, mode);
        if (stream)
            return stream;
        const auto resolved = resolve_existing_path_case_insensitive(path);
        if (!resolved.empty())
            return std::ifstream(resolved, mode);
        return stream;
    }

    std::vector<std::uint8_t> read_file_binary(const std::filesystem::path& path)
    {
        auto file = open_ifstream(path, std::ios::binary | std::ios::ate);
        if (!file)
            return {};
        const auto size = static_cast<std::size_t>(file.tellg());
        file.seekg(0);
        std::vector<std::uint8_t> data(size);
        if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
            data.clear();
        return data;
    }
}
