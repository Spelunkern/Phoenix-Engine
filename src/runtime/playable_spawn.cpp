#include "runtime/playable_spawn.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phoenix::runtime
{
    namespace
    {
        constexpr float kStepHeight = 1.5f;
        constexpr float kCharacterRadius = 0.6f;
        constexpr float kCharacterHeight = 2.2f;

    }

    float character_height_sampler(float worldX, float worldZ, void* userData)
    {
        const auto* ctx = static_cast<const HeightSamplerContext*>(userData);
        float terrainY = ctx->runtime->terrain_height_at(worldX, worldZ);

        if (ctx->collisionMesh && !ctx->collisionMesh->triangles.empty())
        {
            float refY = std::max(terrainY, ctx->lastCharacterY);
            float floorY = ctx->collisionMesh->floor_height_at(
                worldX, worldZ, refY, kStepHeight);
            if (floorY > terrainY)
                terrainY = floorY;
        }

        return terrainY;
    }

    bool character_collision_callback(
        float proposedX,
        float proposedZ,
        float previousX,
        float previousZ,
        float characterY,
        float& outX,
        float& outZ,
        void* userData)
    {
        const auto* collisionMesh = static_cast<const WorldCollisionMesh*>(userData);
        outX = proposedX;
        outZ = proposedZ;
        return collisionMesh->check_collision(previousX, previousZ, outX, outZ,
            characterY, kCharacterHeight, kCharacterRadius);
    }

    PlayableSpawn find_dungeon_playable_spawn(
        const WorldCollisionMesh& collisionMesh,
        float preferredX,
        float preferredY,
        float preferredZ)
    {
        PlayableSpawn best{};
        float bestScore = std::numeric_limits<float>::max();
        for (const auto& tri : collisionMesh.triangles)
        {
            if (tri.normalY < WorldCollisionMesh::kWalkableNormalY)
                continue;

            const float cx = (tri.v0[0] + tri.v1[0] + tri.v2[0]) / 3.0f;
            const float cy = (tri.v0[1] + tri.v1[1] + tri.v2[1]) / 3.0f;
            const float cz = (tri.v0[2] + tri.v1[2] + tri.v2[2]) / 3.0f;

            const float ax = tri.v1[0] - tri.v0[0];
            const float ay = tri.v1[1] - tri.v0[1];
            const float az = tri.v1[2] - tri.v0[2];
            const float bx = tri.v2[0] - tri.v0[0];
            const float by = tri.v2[1] - tri.v0[1];
            const float bz = tri.v2[2] - tri.v0[2];
            const float nx = ay * bz - az * by;
            const float ny = az * bx - ax * bz;
            const float nz = ax * by - ay * bx;
            const float area = std::sqrt(nx * nx + ny * ny + nz * nz) * 0.5f;
            if (area < 0.15f)
                continue;

            const float dx = cx - preferredX;
            const float dy = cy - preferredY;
            const float dz = cz - preferredZ;
            const float score = dx * dx + dz * dz + dy * dy * 0.04f - std::min(area, 64.0f) * 0.25f;
            if (score < bestScore)
            {
                bestScore = score;
                best = { cx, cy + 0.04f, cz, true };
            }
        }
        return best;
    }

    PlayableSpawn find_initial_playable_spawn(
        const WorldCollisionMesh& collisionMesh,
        bool isDungeon)
    {
        float preferredX = 0.0f;
        float preferredY = 0.0f;
        float preferredZ = 0.0f;
        if (isDungeon)
        {
            double sx = 0.0, sy = 0.0, sz = 0.0;
            std::size_t n = 0;
            for (const auto& tri : collisionMesh.triangles)
            {
                if (tri.normalY < WorldCollisionMesh::kWalkableNormalY)
                    continue;
                sx += (tri.v0[0] + tri.v1[0] + tri.v2[0]) / 3.0;
                sy += (tri.v0[1] + tri.v1[1] + tri.v2[1]) / 3.0;
                sz += (tri.v0[2] + tri.v1[2] + tri.v2[2]) / 3.0;
                ++n;
            }
            if (n > 0)
            {
                preferredX = static_cast<float>(sx / static_cast<double>(n));
                preferredY = static_cast<float>(sy / static_cast<double>(n));
                preferredZ = static_cast<float>(sz / static_cast<double>(n));
            }
        }
        return find_dungeon_playable_spawn(collisionMesh, preferredX, preferredY, preferredZ);
    }
}
