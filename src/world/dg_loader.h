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
    };

    struct DgMesh
    {
        std::uint32_t textureIndex{};
        std::string textureName;
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
        std::vector<DgMesh> meshes;
        bool hasCollision{};
        DgCollisionMesh collision;
        bool parsed{};
    };

    DgModel load_dg(const std::filesystem::path& path);
}
