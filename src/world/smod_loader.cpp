#include "world/smod_loader.h"

#include <bit>
#include <fstream>
#include <string>
#include <vector>

namespace phoenix::world
{
    namespace
    {
        std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 4 > data.size())
                return 0;

            return static_cast<std::uint32_t>(data[offset])
                | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
        }

        std::uint16_t read_u16(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 2 > data.size())
                return 0;

            return static_cast<std::uint16_t>(data[offset])
                | (static_cast<std::uint16_t>(data[offset + 1]) << 8);
        }

        float read_f32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<float>(read_u32(data, offset));
        }

        bool read_bytes(const std::vector<std::uint8_t>& data, std::size_t& offset, std::size_t count)
        {
            if (offset > data.size() || count > data.size() - offset)
                return false;

            offset += count;
            return true;
        }

        bool read_string(const std::vector<std::uint8_t>& data, std::size_t& offset, std::string& value)
        {
            if (offset + 4 > data.size())
                return false;

            const auto length = read_u32(data, offset);
            offset += 4;
            if (length > 4096 || offset + length > data.size())
                return false;

            value.assign(reinterpret_cast<const char*>(data.data() + offset), length);
            if (!value.empty() && value.back() == '\0')
                value.pop_back();

            offset += length;
            return true;
        }
    }

    SmodModel load_smod(const std::filesystem::path& path)
    {
        SmodModel model{};

        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return model;

        const auto fileSize = stream.tellg();
        stream.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!stream || data.size() < 4)
            return model;

        std::size_t offset = 0;
        for (float& value : model.center)
        {
            value = read_f32(data, offset);
            offset += 4;
        }
        model.radius = read_f32(data, offset);
        offset += 4;

        // View bounding box: min/max Vector3. It is useful later for culling, but not needed for first render.
        if (!read_bytes(data, offset, 24) || offset + 4 > data.size())
            return model;

        const auto meshCount = read_u32(data, offset);
        offset += 4;
        if (meshCount > 10000)
            return model;

        model.meshes.reserve(meshCount);
        for (std::uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
        {
            SmodMesh mesh{};
            if (!read_string(data, offset, mesh.textureName) || offset + 4 > data.size())
                return {};

            const auto vertexCount = read_u32(data, offset);
            offset += 4;
            if (vertexCount > 1000000 || offset + static_cast<std::size_t>(vertexCount) * 36 > data.size())
                return {};

            mesh.vertices.reserve(vertexCount);
            for (std::uint32_t i = 0; i < vertexCount; ++i)
            {
                SmodVertex vertex{};
                for (float& value : vertex.position)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : vertex.normal)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }

                // Bone id is always -1 for SMODs.
                offset += 4;
                vertex.uv[0] = read_f32(data, offset);
                offset += 4;
                vertex.uv[1] = read_f32(data, offset);
                offset += 4;
                mesh.vertices.push_back(vertex);
            }

            if (offset + 4 > data.size())
                return {};

            const auto faceCount = read_u32(data, offset);
            offset += 4;
            if (faceCount > 1000000 || offset + static_cast<std::size_t>(faceCount) * 6 > data.size())
                return {};

            mesh.faces.reserve(faceCount);
            for (std::uint32_t i = 0; i < faceCount; ++i)
            {
                SmodFace face{};
                face.indices[0] = read_u16(data, offset);
                offset += 2;
                face.indices[1] = read_u16(data, offset);
                offset += 2;
                face.indices[2] = read_u16(data, offset);
                offset += 2;
                mesh.faces.push_back(face);
            }

            model.meshes.push_back(std::move(mesh));
        }

        // ---- Collision data (after visual meshes) ----
        // Bounding box: 6 floats (min XYZ, max XYZ).
        if (offset + 28 <= data.size())
        {
            for (float& value : model.collision.bboxMin)
            {
                value = read_f32(data, offset);
                offset += 4;
            }
            for (float& value : model.collision.bboxMax)
            {
                value = read_f32(data, offset);
                offset += 4;
            }

            const auto collisionType = read_u32(data, offset);
            offset += 4;

            if (collisionType == 1 && offset + 4 <= data.size())
            {
                const auto vertexCount = read_u32(data, offset);
                offset += 4;
                if (vertexCount <= 250000
                    && offset + static_cast<std::size_t>(vertexCount) * 12 + 4 <= data.size())
                {
                    model.collision.vertices.resize(static_cast<std::size_t>(vertexCount) * 3);
                    for (std::uint32_t i = 0; i < vertexCount; ++i)
                    {
                        model.collision.vertices[i * 3 + 0] = read_f32(data, offset); offset += 4;
                        model.collision.vertices[i * 3 + 1] = read_f32(data, offset); offset += 4;
                        model.collision.vertices[i * 3 + 2] = read_f32(data, offset); offset += 4;
                    }

                    const auto faceCount = read_u32(data, offset);
                    offset += 4;
                    if (faceCount <= 500000
                        && offset + static_cast<std::size_t>(faceCount) * 6 <= data.size())
                    {
                        model.collision.indices.resize(static_cast<std::size_t>(faceCount) * 3);
                        for (std::uint32_t i = 0; i < faceCount; ++i)
                        {
                            model.collision.indices[i * 3 + 0] = read_u16(data, offset); offset += 2;
                            model.collision.indices[i * 3 + 1] = read_u16(data, offset); offset += 2;
                            model.collision.indices[i * 3 + 2] = read_u16(data, offset); offset += 2;
                        }
                        model.hasCollision = true;
                    }
                }
            }
        }

        model.parsed = true;
        return model;
    }
}
