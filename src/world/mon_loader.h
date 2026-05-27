#pragma once

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct MonsterPart
    {
        std::string meshFileName;
        std::string textureFileName;
    };

    struct MonsterDefinition
    {
        std::string name;
        std::array<std::string, 9> animationSlots;
        std::vector<std::string> animationFileNames;
        std::vector<std::string> soundFileNames;
        std::vector<MonsterPart> parts;
        float scale{ 1.0f };
        float height{ 0.0f };
    };

    struct MonsterTable
    {
        std::vector<MonsterDefinition> monsters;
        std::uint32_t declaredCount{};
        bool parsed{};
    };

    MonsterTable load_monster_table(const std::filesystem::path& path);
}
