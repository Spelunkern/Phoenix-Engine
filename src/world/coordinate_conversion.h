#pragma once

#include <algorithm>

namespace phoenix::world
{
    // Shaiya's authored world is left-handed.  OpenGL and the Phoenix camera
    // use a right-handed world, so X is reflected exactly once at the source
    // data boundary.  Outdoor coordinates are also centred around the origin;
    // dungeon coordinates are not (halfMap == 0), but still need reflection.
    inline constexpr float source_to_world_x(float sourceX, float halfMap)
    {
        return halfMap - sourceX;
    }

    inline constexpr float source_to_world_z(float sourceZ, float halfMap)
    {
        return sourceZ - halfMap;
    }

    inline constexpr float world_to_source_u(float worldX, float mapSize)
    {
        return mapSize > 0.0f
            ? std::clamp((mapSize * 0.5f - worldX) / mapSize, 0.0f, 1.0f)
            : 0.0f;
    }

    inline constexpr float world_to_source_v(float worldZ, float mapSize)
    {
        return mapSize > 0.0f
            ? std::clamp((worldZ + mapSize * 0.5f) / mapSize, 0.0f, 1.0f)
            : 0.0f;
    }

    inline void mirror_source_direction_x(float direction[3])
    {
        direction[0] = -direction[0];
    }

    inline void mirror_source_box_x(float& minX, float& maxX, float halfMap)
    {
        const float sourceMin = std::min(minX, maxX);
        const float sourceMax = std::max(minX, maxX);
        minX = source_to_world_x(sourceMax, halfMap);
        maxX = source_to_world_x(sourceMin, halfMap);
    }
}
