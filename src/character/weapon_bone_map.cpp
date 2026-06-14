#include "character/weapon_bone_map.h"

#include "assets/data_index.h"

#include <unordered_map>

namespace phoenix::character
{
    void apply_default_attach_bones(const std::string& prefix, WeaponType weapon,
                                    int& weaponBone, int& shieldBone, int& dualWeaponBone,
                                    float dualOffsetPos[3], float dualOffsetRotDeg[3])
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

        // Off-hand (dual) weapon defaults, used only by dual-wield types: its own
        // bone plus a local-space offset (X, Y, Z) and rotation (degrees, X, Y, Z).
        struct Dual { int bone; float pos[3]; float rotDeg[3]; };
        static const std::unordered_map<std::string, Dual> dualTable = {
            // Dual swords (fighters)
            { "humf", { 15, { 0.0f, 0.0f, 0.0f }, { 180.0f, 0.0f, 0.0f } } },
            { "huwf", { 17, { 0.0f, 0.0f, 0.0f }, { 180.0f, 0.0f, 0.0f } } },
            { "demf", { 15, { 0.0f, 0.0f, 0.0f }, { 180.0f, 180.0f, 0.0f } } },
            { "dewf", { 18, { 0.030f, 0.0f, 0.0f }, { 180.0f, 180.0f, 0.0f } } },
            // Claws (rangers)
            { "elmr", { 14, { 0.325f, 0.0f, 0.0f }, { 0.0f, -100.0f, 0.500f } } },
            { "elwr", { 14, { 0.325f, 0.0f, 0.0f }, { 0.0f, -100.0f, 0.500f } } },
            { "vimr", { 14, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
            { "viwr", { 20, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } } },
        };

        // Default the off-hand fine-tune off; only dual-wield types set it below.
        for (int i = 0; i < 3; ++i) { dualOffsetPos[i] = 0.0f; dualOffsetRotDeg[i] = 0.0f; }

        const std::string key = assets::lower_ascii(prefix);
        const auto it = table.find(key);
        if (it == table.end())
            return; // unknown prefix: leave whatever default is set

        weaponBone = it->second.weapon;
        shieldBone = it->second.shield;
        // The off-hand dual weapon rides the left-wrist bone (same as shields)
        // unless overridden per character below.
        dualWeaponBone = it->second.shield;

        // Ranged weapons use a dedicated bone on the ranged classes.
        const bool isBow = (weapon == WeaponType::Bow);
        if (key == "elmr" && (isBow || weapon == WeaponType::Crossbow)) weaponBone = 14;
        else if (key == "elwr" && (isBow || weapon == WeaponType::Crossbow)) weaponBone = 18;
        else if (key == "demr" && (isBow || weapon == WeaponType::Javelin)) weaponBone = 14;
        else if (key == "dewr" && (isBow || weapon == WeaponType::Javelin)) weaponBone = 18;
        // Claws use a dedicated primary bone on the elf rangers.
        else if (key == "elmr" && weapon == WeaponType::Claw) weaponBone = 22;
        else if (key == "elwr" && weapon == WeaponType::Claw) weaponBone = 26;

        // Dual-wield off-hand: bone + local offset/rotation per character.
        if (weapon_type_dual_wield(weapon))
        {
            if (const auto dit = dualTable.find(key); dit != dualTable.end())
            {
                dualWeaponBone = dit->second.bone;
                for (int i = 0; i < 3; ++i)
                {
                    dualOffsetPos[i] = dit->second.pos[i];
                    dualOffsetRotDeg[i] = dit->second.rotDeg[i];
                }
            }
        }
    }
}
