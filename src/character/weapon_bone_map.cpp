#include "character/weapon_bone_map.h"

#include "assets/data_index.h"

#include <unordered_map>

namespace phoenix::character
{
    void apply_default_attach_bones(const std::string& prefix, WeaponType weapon,
                                    int& weaponBone, int& shieldBone)
    {
        struct Bones { int weapon; int shield; };
        static const std::unordered_map<std::string, Bones> table = {
            // Human
            { "humf", { 24, 10 } }, { "huwf", { 27, 13 } },
            { "humm", { 24, 10 } }, { "huwm", { 27, 13 } },
            // Elf
            { "elmm", { 25, 13 } }, { "elwm", { 27, 14 } },
            { "elmr", { 23, 10 } }, { "elwr", { 27, 14 } },
            // Deatheater
            { "demf", { 25, 10 } }, { "dewf", { 28, 14 } },
            { "demr", { 24, 10 } }, { "dewr", { 28, 14 } },
            // Vile
            { "vimm", { 22, 10 } }, { "viwm", { 28, 16 } },
            { "vimr", { 24, 10 } }, { "viwr", { 30, 16 } },
        };

        const std::string key = assets::lower_ascii(prefix);
        const auto it = table.find(key);
        if (it == table.end())
            return; // unknown prefix: leave whatever default is set

        weaponBone = it->second.weapon;
        shieldBone = it->second.shield;

        // Ranged weapons use a dedicated bone on the ranged classes.
        const bool isBow = (weapon == WeaponType::Bow);
        if (key == "elmr" && (isBow || weapon == WeaponType::Crossbow)) weaponBone = 14;
        else if (key == "elwr" && (isBow || weapon == WeaponType::Crossbow)) weaponBone = 18;
        else if (key == "demr" && (isBow || weapon == WeaponType::Javelin)) weaponBone = 14;
        else if (key == "dewr" && (isBow || weapon == WeaponType::Javelin)) weaponBone = 18;
    }
}
