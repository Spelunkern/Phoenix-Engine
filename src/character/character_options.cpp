#include "character/character_options.h"

#include "assets/data_index.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

namespace phoenix::character
{
    namespace
    {
        std::string lower_ascii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::vector<int> scan_csv_indices(const std::filesystem::path& csvPath)
        {
            std::vector<int> indices;
            auto file = phoenix::assets::open_ifstream(csvPath);
            if (!file)
                return indices;
            std::string line;
            std::getline(file, line);
            while (std::getline(file, line))
            {
                if (line.empty())
                    continue;
                std::string clean;
                clean.reserve(line.size());
                for (char c : line)
                    if (c != '"') clean += c;
                auto comma = clean.find(',');
                if (comma == std::string::npos) continue;
                indices.push_back(std::atoi(clean.substr(0, comma).c_str()));
            }
            std::ranges::sort(indices);
            indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
            return indices;
        }
    }

    std::vector<phoenix::ui::CharacterOption> scan_character_options(
        const std::filesystem::path& dataRoot)
    {
        const auto characterRoot = phoenix::assets::resolve_existing_path_case_insensitive(dataRoot / "Character");
        if (characterRoot.empty() || !std::filesystem::exists(characterRoot))
            return {};

        constexpr std::string_view partFiles[] = { "upper", "lower", "hand", "foot", "helmet", "face", "hair" };

        struct Temp
        {
            std::string raceFolder;
            std::string prefix;
            std::vector<int> upper, lower, hand, foot, helmet, face, hair;
        };

        std::map<std::string, Temp> byKey;
        for (const auto& raceEntry : std::filesystem::directory_iterator(characterRoot))
        {
            if (!raceEntry.is_directory())
                continue;
            const auto raceFolder = raceEntry.path().filename().string();
            for (const auto& entry : std::filesystem::directory_iterator(raceEntry.path()))
            {
                if (!entry.is_regular_file())
                    continue;
                auto ext = lower_ascii(entry.path().extension().string());
                if (ext != ".csv")
                    continue;
                auto stem = lower_ascii(entry.path().stem().string());
                if (stem.find("_action") != std::string::npos)
                    continue;

                for (const auto partName : partFiles)
                {
                    auto suffix = "_" + std::string(partName);
                    if (!stem.ends_with(suffix))
                        continue;
                    auto prefix = stem.substr(0, stem.size() - suffix.size());
                    auto key = raceFolder + "|" + prefix;
                    auto& temp = byKey[key];
                    temp.raceFolder = raceFolder;
                    temp.prefix = prefix;
                    auto indices = scan_csv_indices(entry.path());
                    if (partName == "upper") temp.upper = std::move(indices);
                    else if (partName == "lower") temp.lower = std::move(indices);
                    else if (partName == "hand") temp.hand = std::move(indices);
                    else if (partName == "foot") temp.foot = std::move(indices);
                    else if (partName == "helmet") temp.helmet = std::move(indices);
                    else if (partName == "face") temp.face = std::move(indices);
                    else if (partName == "hair") temp.hair = std::move(indices);
                    break;
                }
            }
        }

        std::vector<phoenix::ui::CharacterOption> options;
        options.reserve(byKey.size());
        for (auto& [_, temp] : byKey)
        {
            if (temp.face.empty() || temp.hair.empty())
                continue;
            phoenix::ui::CharacterOption option{};
            option.raceFolder = temp.raceFolder;
            option.prefix = temp.prefix;
            option.label = temp.raceFolder + " / " + temp.prefix;
            option.upperIndices = std::move(temp.upper);
            option.lowerIndices = std::move(temp.lower);
            option.handIndices = std::move(temp.hand);
            option.footIndices = std::move(temp.foot);
            option.helmetIndices = std::move(temp.helmet);
            option.faceIndices = std::move(temp.face);
            option.hairIndices = std::move(temp.hair);
            options.push_back(std::move(option));
        }

        std::ranges::sort(options, [](const auto& lhs, const auto& rhs) {
            return lhs.label < rhs.label;
        });
        return options;
    }
}
