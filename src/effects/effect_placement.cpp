#include "effects/effect_placement.h"

namespace phoenix::effects
{
    std::uint32_t place_portal_effects(
        EffectManager& effectManager,
        const phoenix::runtime::PhoenixRuntime& runtime)
    {
        static const EffectDefinition* portalDef = []() -> const EffectDefinition* {
            for (const auto& def : preset_catalog())
                if (def.name == "Portal")
                    return &def;
            return nullptr;
        }();
        if (!portalDef)
            return 0;

        const float mapSize = static_cast<float>(runtime.state().world.mapSize);
        const float halfMap = runtime.state().world.isDungeon ? 0.0f : mapSize * 0.5f;
        std::uint32_t portalCount = 0;
        for (const auto& portal : runtime.state().world.portals)
        {
            const float cx = (portal.box.min[0] + portal.box.max[0]) * 0.5f - halfMap;
            const float cy = (portal.box.min[1] + portal.box.max[1]) * 0.5f;
            const float cz = (portal.box.min[2] + portal.box.max[2]) * 0.5f - halfMap;
            EffectAnchor anchor{};
            anchor.position[0] = cx;
            anchor.position[1] = cy + 1.8f;
            anchor.position[2] = cz;
            constexpr float s = 2.0f;
            anchor.basis[0] = s;  anchor.basis[1] = 0;  anchor.basis[2] = 0;
            anchor.basis[3] = 0;  anchor.basis[4] = 0;  anchor.basis[5] = -s;
            anchor.basis[6] = 0;  anchor.basis[7] = s;  anchor.basis[8] = 0;
            effectManager.spawn(*portalDef, anchor);
            ++portalCount;
        }
        return portalCount;
    }
}
