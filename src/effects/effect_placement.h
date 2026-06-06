#pragma once

#include "effects/effect_system.h"
#include "runtime/phoenix_runtime.h"

#include <cstdint>

namespace phoenix::effects
{
    std::uint32_t place_portal_effects(
        EffectManager& effectManager,
        const phoenix::runtime::PhoenixRuntime& runtime);
}
