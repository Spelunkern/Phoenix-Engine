#pragma once

#include "character/character_system.h"   // WeaponType

#include <string>

namespace phoenix::character
{
    // Apply the per-character default weapon/shield attach-bone indices (recovered
    // from the retail skeletons). Archer/ranger/hunter classes route ranged
    // weapons (bow/crossbow/javelin) to a dedicated bone. Values are overridable
    // live from the UI and re-applied on every (re)load.
    void apply_default_attach_bones(const std::string& prefix, WeaponType weapon,
                                    int& weaponBone, int& shieldBone);
}
