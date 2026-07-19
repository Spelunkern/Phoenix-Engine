#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct EftMeshVertex
    {
        float position[3]{};
        float uv[2]{};
    };

    // One keyframe of the mesh's vertex animation: all vertex positions/UVs at
    // tick `key`. Frames are sparse (only authored keys are stored) and the
    // engine interpolates between them the same way the retail 3DE format does.
    struct EftMeshFrame
    {
        std::int32_t key{};
        std::vector<EftMeshVertex> vertices;
    };

    struct EftMeshFace
    {
        std::uint16_t indices[3]{};
    };

    // A single effect particle mesh (.3DE), referenced by EftEffect::meshIndex
    // through the owning EftLibrary's meshNames table.
    struct EftMesh
    {
        std::string textureName;
        std::vector<EftMeshVertex> vertices;
        std::vector<EftMeshFace> faces;
        // 0 when the mesh has no vertex animation.
        std::int32_t maxKeyframe{};
        std::vector<EftMeshFrame> frames;
        bool parsed{};
    };

    EftMesh load_eft_mesh(const std::filesystem::path& path);
}
