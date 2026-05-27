#pragma once

#include <filesystem>

namespace phoenix::world
{
    struct ManiAnimation
    {
        float rotationAxis[3]{};
        float animationSpeed{};
        bool enableRotation{};
        bool parsed{};
    };

    ManiAnimation load_mani(const std::filesystem::path& path);
}
