#pragma once

#include "character/character_system.h"   // WeaponType

#include <string>

namespace phoenix::character
{
    // Apply the per-character default weapon/shield attach-bone indices (recovered
    // from the retail skeletons). Archer/ranger/hunter classes route ranged
    // weapons (bow/crossbow/javelin) to a dedicated bone. For dual-wield types
    // (DualSword/Claw) the off-hand copy also gets its own bone plus a fine
    // offset/rotation (in its bone's local space). Values are overridable live
    // from the UI and re-applied on every (re)load. dualOffsetPos/RotDeg are
    // (X, Y, Z); rotation is in degrees.
    void apply_default_attach_bones(const std::string& prefix, WeaponType weapon,
                                    int& weaponBone, int& shieldBone, int& dualWeaponBone,
                                    float dualOffsetPos[3], float dualOffsetRotDeg[3]);
}
