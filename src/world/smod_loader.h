#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct SmodVertex
    {
        float position[3]{};
        float normal[3]{};
        float uv[2]{};
    };

    struct SmodFace
    {
        std::uint16_t indices[3]{};
    };

    struct SmodMesh
    {
        std::string textureName;
        std::vector<SmodVertex> vertices;
        std::vector<std::vector<SmodVertex>> animationFrames;
        std::vector<SmodFace> faces;
    };

    struct SmodCollisionMesh
    {
        float bboxMin[3]{};
        float bboxMax[3]{};
        std::vector<float> vertices; // flat: x,y,z per vertex
        std::vector<std::uint32_t> indices; // 3 per face
    };

    struct SmodModel
    {
        float center[3]{};
        float radius{};
        std::uint32_t frameCount{ 1 };
        bool vertexAnimated{};
        std::vector<SmodMesh> meshes;
        bool hasCollision{};
        SmodCollisionMesh collision;
        bool parsed{};
    };

    SmodModel load_smod(const std::filesystem::path& path);
}
