#pragma once

#include "runtime/phoenix_runtime.h"

namespace phoenix::runtime
{
    struct HeightSamplerContext
    {
        const PhoenixRuntime* runtime{};
        const WorldCollisionMesh* collisionMesh{};
        mutable float lastCharacterY{};
    };

    struct PlayableSpawn
    {
        float x{};
        float y{};
        float z{};
        bool valid{};
    };

    float character_height_sampler(float worldX, float worldZ, void* userData);

    bool character_collision_callback(
        float proposedX,
        float proposedZ,
        float previousX,
        float previousZ,
        float characterY,
        float& outX,
        float& outZ,
        void* userData);

    PlayableSpawn find_initial_playable_spawn(
        const WorldCollisionMesh& collisionMesh,
        bool isDungeon);

    PlayableSpawn find_dungeon_playable_spawn(
        const WorldCollisionMesh& collisionMesh,
        float preferredX,
        float preferredY,
        float preferredZ);
}
