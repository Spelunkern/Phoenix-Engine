#include "world/portal_runtime.h"

#include <algorithm>
#include <cstdlib>

namespace phoenix::world
{
    std::optional<PortalActivation> check_portal_activation(
        const phoenix::runtime::PhoenixRuntime& runtime,
        const std::vector<std::string>& mapNames,
        float characterX,
        float characterY,
        float characterZ)
    {
        const float mapSize = static_cast<float>(runtime.state().world.mapSize);
        const float halfMap = runtime.state().world.isDungeon ? 0.0f : mapSize * 0.5f;
        for (const auto& portal : runtime.state().world.portals)
        {
            const float minX = portal.box.min[0] - halfMap;
            const float maxX = portal.box.max[0] - halfMap;
            const float minZ = portal.box.min[2] - halfMap;
            const float maxZ = portal.box.max[2] - halfMap;
            const float minY = portal.box.min[1] - 2.0f;
            const float maxY = portal.box.max[1] + 4.0f;
            if (characterX < std::min(minX, maxX) || characterX > std::max(minX, maxX)
                || characterZ < std::min(minZ, maxZ) || characterZ > std::max(minZ, maxZ)
                || characterY < std::min(minY, maxY) || characterY > std::max(minY, maxY))
            {
                continue;
            }

            PortalActivation activation{};
            activation.destinationMapId = static_cast<int>(portal.mapId);
            for (std::size_t i = 0; i < mapNames.size(); ++i)
            {
                // Map names are "worldN" — extract the number after the "world" prefix.
                const auto& name = mapNames[i];
                const char* s = name.c_str();
                if (name.size() > 5 && name.substr(0, 5) == "world")
                    s += 5;
                char* end = nullptr;
                const long n = std::strtol(s, &end, 10);
                if (end != s && *end == '\0' && n == activation.destinationMapId)
                {
                    activation.destinationMapIndex = i;
                    break;
                }
            }

            activation.teleport.x = portal.destinationPosition[0];
            activation.teleport.y = portal.destinationPosition[1];
            activation.teleport.z = portal.destinationPosition[2];
            activation.teleport.hasDestination =
                activation.teleport.x != 0.0f
                || activation.teleport.y != 0.0f
                || activation.teleport.z != 0.0f;
            return activation;
        }

        return std::nullopt;
    }
}
