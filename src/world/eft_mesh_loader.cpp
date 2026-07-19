#include "world/eft_mesh_loader.h"

#include "assets/data_index.h"
#include "world/eft_binary_reader.h"

namespace phoenix::world
{
    namespace
    {
        using phoenix::world::detail::EftReader;
    }

    EftMesh load_eft_mesh(const std::filesystem::path& path)
    {
        EftMesh mesh{};

        const auto data = phoenix::assets::read_file_binary(path);
        if (data.empty())
            return mesh;

        EftReader reader{ data };

        mesh.textureName = reader.string(4096);

        const auto vertexCount = reader.count(1000000);
        mesh.vertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount && reader.ok; ++i)
        {
            EftMeshVertex vertex{};
            reader.vec3(vertex.position);
            reader.i32(); // Bone id: unused, effect meshes are not skinned.
            reader.vec2(vertex.uv);
            mesh.vertices.push_back(vertex);
        }

        const auto faceCount = reader.count(1000000);
        mesh.faces.reserve(faceCount);
        for (std::uint32_t i = 0; i < faceCount && reader.ok; ++i)
        {
            EftMeshFace face{};
            face.indices[0] = reader.u16();
            face.indices[1] = reader.u16();
            face.indices[2] = reader.u16();
            if (reader.ok
                && (face.indices[0] >= vertexCount || face.indices[1] >= vertexCount
                    || face.indices[2] >= vertexCount))
            {
                reader.ok = false;
                break;
            }
            mesh.faces.push_back(face);
        }

        // Vertex animation is optional and only present when trailing bytes
        // remain after the face list.
        if (reader.ok && reader.remaining())
            mesh.maxKeyframe = reader.i32();

        if (reader.ok && reader.remaining())
        {
            const auto frameCount = reader.count(100000);
            mesh.frames.reserve(frameCount);
            for (std::uint32_t i = 0; i < frameCount && reader.ok; ++i)
            {
                EftMeshFrame frame{};
                frame.key = reader.i32();
                frame.vertices.reserve(vertexCount);
                for (std::uint32_t v = 0; v < vertexCount && reader.ok; ++v)
                {
                    EftMeshVertex vertex{};
                    reader.vec3(vertex.position);
                    reader.vec2(vertex.uv);
                    frame.vertices.push_back(vertex);
                }
                mesh.frames.push_back(std::move(frame));
            }
        }

        mesh.parsed = reader.ok;
        if (!mesh.parsed)
        {
            mesh.vertices.clear();
            mesh.faces.clear();
            mesh.frames.clear();
            mesh.maxKeyframe = 0;
        }

        return mesh;
    }
}
