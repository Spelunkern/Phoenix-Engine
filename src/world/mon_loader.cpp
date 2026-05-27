#include "world/mon_loader.h"

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace phoenix::world
{
    namespace
    {
        bool is_placeholder(const std::string& value)
        {
            return value.empty() || value == "LOAD" || value == "load";
        }

        std::string next_csv_field(const std::string& line, std::size_t& pos)
        {
            if (pos >= line.size())
                return {};

            if (line[pos] == '"')
            {
                ++pos;
                std::string result;
                while (pos < line.size())
                {
                    if (line[pos] == '"')
                    {
                        if (pos + 1 < line.size() && line[pos + 1] == '"')
                        {
                            result += '"';
                            pos += 2;
                        }
                        else
                        {
                            ++pos; // closing quote
                            if (pos < line.size() && line[pos] == ',')
                                ++pos;
                            return result;
                        }
                    }
                    else
                    {
                        result += line[pos++];
                    }
                }
                return result;
            }

            auto comma = line.find(',', pos);
            if (comma == std::string::npos) comma = line.size();
            auto token = line.substr(pos, comma - pos);
            pos = comma < line.size() ? comma + 1 : line.size();
            return token;
        }

        std::string extract_json_string_value(const std::string& json, std::size_t keyEnd)
        {
            // After the key name, find ":"value"
            auto colon = json.find(':', keyEnd);
            if (colon == std::string::npos) return {};
            auto quote1 = json.find('"', colon + 1);
            if (quote1 == std::string::npos) return {};
            auto quote2 = json.find('"', quote1 + 1);
            if (quote2 == std::string::npos) return {};
            return json.substr(quote1 + 1, quote2 - quote1 - 1);
        }

        void parse_objects_json(const std::string& json, std::vector<MonsterPart>& parts)
        {
            std::size_t pos = 0;
            while (pos < json.size())
            {
                auto meshKey = json.find("meshName", pos);
                if (meshKey == std::string::npos) break;
                auto meshValue = extract_json_string_value(json, meshKey + 8);

                auto texKey = json.find("textureName", meshKey + 8);
                if (texKey == std::string::npos) break;
                auto texValue = extract_json_string_value(json, texKey + 11);

                MonsterPart part{};
                part.meshFileName = std::move(meshValue);
                part.textureFileName = std::move(texValue);
                if (!part.meshFileName.empty() && !part.textureFileName.empty())
                    parts.push_back(std::move(part));

                pos = texKey + 11;
            }
        }
    }

    MonsterTable load_monster_table(const std::filesystem::path& path)
    {
        MonsterTable table{};

        std::ifstream file(path);
        if (!file)
            return table;

        std::string line;
        std::getline(file, line); // skip header
        // RecordIndex,Signature,Format,Name,Unknown,
        // WalkAnimation(5),RunAnimation(6),JumpAttack1Animation(7),Attack2Animation(8),Attack3Animation(9),
        // DeathAnimation(10),BreathAnimation(11),DamageAnimation(12),IdleAnimation(13),
        // Attack1Wav(14),...DeathWav(17),...AttachEffect(22),
        // ObjectsCount(23),Objects(24),Height(25),EffectsCount(26),Effects(27)

        while (std::getline(file, line))
        {
            if (line.empty())
                continue;

            std::size_t pos = 0;
            next_csv_field(line, pos); // RecordIndex
            next_csv_field(line, pos); // Signature
            next_csv_field(line, pos); // Format

            MonsterDefinition monster{};
            monster.name = next_csv_field(line, pos); // Name
            next_csv_field(line, pos); // Unknown

            // Animation slots 0-8:  Walk(0), Run(1), JumpAttack1(2), Attack2(3), Attack3(4),
            //                       Death(5), Breath(6), Damage(7), Idle(8)
            const std::string_view slotNames[] = {
                "Walk", "Run", "JumpAttack1", "Attack2", "Attack3",
                "Death", "Breath", "Damage", "Idle"
            };
            (void)slotNames;
            for (int i = 0; i < 9; ++i)
            {
                auto anim = next_csv_field(line, pos);
                monster.animationSlots[i] = anim;
                if (!is_placeholder(anim))
                    monster.animationFileNames.push_back(std::move(anim));
            }

            // Sound files: Attack1Wav..DeathWav (4 sounds)
            for (int i = 0; i < 4; ++i)
            {
                auto sound = next_csv_field(line, pos);
                if (!is_placeholder(sound))
                    monster.soundFileNames.push_back(std::move(sound));
            }

            // Effects: Attack1Effect..AttachEffect (5 fields)
            for (int i = 0; i < 5; ++i)
                next_csv_field(line, pos);

            next_csv_field(line, pos); // ObjectsCount
            auto objects = next_csv_field(line, pos); // Objects (JSON array)
            parse_objects_json(objects, monster.parts);

            auto heightStr = next_csv_field(line, pos); // Height
            if (!heightStr.empty())
            {
                monster.height = std::strtof(heightStr.c_str(), nullptr);
                monster.scale = monster.height;
            }

            table.monsters.push_back(std::move(monster));
        }

        table.declaredCount = static_cast<std::uint32_t>(table.monsters.size());
        table.parsed = !table.monsters.empty();
        return table;
    }
}
