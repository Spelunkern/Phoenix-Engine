#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct DgVertex
    {
        float position[3]{};
        float normal[3]{};
        float uv[2]{};
        float lightmapUv[2]{};
    };

    struct DgMesh
    {
        std::uint32_t textureIndex{};
        std::string textureName;
        // Index into the dungeon's lightmap pages (<name>_L<i>.dds), -1 = none.
        std::int32_t lightmapIndex{ -1 };
        std::vector<DgVertex> vertices;
        std::vector<std::uint32_t> indices;
    };

    struct DgCollisionMesh
    {
        std::vector<float> vertices; // flat x,y,z
        std::vector<std::uint32_t> indices; // 3 per face
    };

    struct DgModel
    {
        float center[3]{};
        float extent[3]{};
        std::vector<std::string> textures;
        std::uint32_t lightmapCount{};
        std::vector<DgMesh> meshes;
        bool hasCollision{};
        DgCollisionMesh collision;
        bool parsed{};
    };

    DgModel load_dg(const std::filesystem::path& path);
}
