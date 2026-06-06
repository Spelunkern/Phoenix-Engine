#pragma once

#include "runtime/phoenix_runtime.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct PendingTeleport
    {
        float x{};
        float y{};
        float z{};
        bool hasDestination{};
    };

    struct PortalActivation
    {
        int destinationMapId{};
        std::optional<std::size_t> destinationMapIndex;
        PendingTeleport teleport;
    };

    std::optional<PortalActivation> check_portal_activation(
        const phoenix::runtime::PhoenixRuntime& runtime,
        const std::vector<std::string>& mapNames,
        float characterX,
        float characterY,
        float characterZ);
}
