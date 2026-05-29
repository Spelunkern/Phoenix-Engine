#include "world/dg_loader.h"

#include "core/logging.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace phoenix::world
{
    namespace
    {
        struct Reader
        {
            const std::vector<std::uint8_t>& data;
            std::size_t offset{};
            bool ok{ true };

            bool can_read(std::size_t bytes)
            {
                if (!ok || offset > data.size() || bytes > data.size() - offset)
                {
                    ok = false;
                    return false;
                }
                return true;
            }

            std::uint32_t u32()
            {
                if (!can_read(4))
                    return 0;
                const auto value = static_cast<std::uint32_t>(data[offset])
                    | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
                    | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
                    | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
                offset += 4;
                return value;
            }

            std::uint16_t u16()
            {
                if (!can_read(2))
                    return 0;
                const auto value = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(data[offset])
                    | static_cast<std::uint16_t>(data[offset + 1] << 8));
                offset += 2;
                return value;
            }

            float f32()
            {
                return std::bit_cast<float>(u32());
            }

            std::string string256()
            {
                if (!can_read(256))
                    return {};

                std::size_t length{};
                while (length < 256 && data[offset + length] != 0)
                    ++length;

                std::string value(reinterpret_cast<const char*>(data.data() + offset), length);
                offset += 256;
                return value;
            }

            void skip(std::size_t bytes)
            {
                if (can_read(bytes))
                    offset += bytes;
            }
        };

        bool finite_reasonable(float value, float limit)
        {
            return std::isfinite(value) && std::abs(value) <= limit;
        }

        void read_vec3(Reader& reader, float* out)
        {
            out[0] = reader.f32();
            out[1] = reader.f32();
            out[2] = reader.f32();
        }

        void skip_vec3(Reader& reader)
        {
            reader.skip(12);
        }

        void read_bbox(Reader& reader, float* center, float* extent)
        {
            float minv[3]{};
            float maxv[3]{};
            read_vec3(reader, minv);
            read_vec3(reader, maxv);
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                center[axis] = (minv[axis] + maxv[axis]) * 0.5f;
                extent[axis] = std::abs(maxv[axis] - minv[axis]) * 0.5f;
            }
        }

        void skip_bbox(Reader& reader)
        {
            reader.skip(24);
        }

        bool valid_count(std::uint32_t count, std::uint32_t maxCount)
        {
            return count <= maxCount;
        }

        bool read_mesh(Reader& reader, std::uint32_t textureIndex, DgMesh& mesh)
        {
            const auto lightmapIndex = reader.u32();
            (void)lightmapIndex;

            const auto vertexCount = reader.u32();
            if (!valid_count(vertexCount, 250000))
                return false;

            mesh.textureIndex = textureIndex;
            mesh.vertices.reserve(vertexCount);
            for (std::uint32_t i = 0; i < vertexCount; ++i)
            {
                DgVertex vertex{};
                read_vec3(reader, vertex.position);
                read_vec3(reader, vertex.normal);
                reader.skip(4); // Bone id.
                vertex.uv[0] = reader.f32();
                vertex.uv[1] = reader.f32();
                reader.skip(8); // Lightmap UV.

                if (!finite_reasonable(vertex.position[0], 1000000.0f)
                    || !finite_reasonable(vertex.position[1], 1000000.0f)
                    || !finite_reasonable(vertex.position[2], 1000000.0f))
                {
                    reader.ok = false;
                    return false;
                }

                if (!finite_reasonable(vertex.normal[0], 4.0f)
                    || !finite_reasonable(vertex.normal[1], 4.0f)
                    || !finite_reasonable(vertex.normal[2], 4.0f))
                {
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 1.0f;
                    vertex.normal[2] = 0.0f;
                }

                mesh.vertices.push_back(vertex);
            }

            const auto faceCount = reader.u32();
            if (!valid_count(faceCount, 500000))
                return false;

            mesh.indices.reserve(static_cast<std::size_t>(faceCount) * 3u);
            for (std::uint32_t i = 0; i < faceCount; ++i)
            {
                const auto a = reader.u16();
                const auto b = reader.u16();
                const auto c = reader.u16();
                if (a >= vertexCount || b >= vertexCount || c >= vertexCount)
                {
                    reader.ok = false;
                    return false;
                }
                mesh.indices.push_back(a);
                mesh.indices.push_back(b);
                mesh.indices.push_back(c);
            }

            return reader.ok;
        }

        bool read_collision_mesh(Reader& reader, DgModel& model)
        {
            const auto vertexCount = reader.u32();
            if (!valid_count(vertexCount, 250000))
                return false;
            const auto vertexBase = model.collision.vertices.size() / 3;
            model.collision.vertices.resize(model.collision.vertices.size() + static_cast<std::size_t>(vertexCount) * 3);
            for (std::uint32_t i = 0; i < vertexCount; ++i)
            {
                model.collision.vertices[(vertexBase + i) * 3 + 0] = reader.f32();
                model.collision.vertices[(vertexBase + i) * 3 + 1] = reader.f32();
                model.collision.vertices[(vertexBase + i) * 3 + 2] = reader.f32();
            }

            const auto faceCount = reader.u32();
            if (!valid_count(faceCount, 500000))
                return false;
            model.collision.indices.reserve(model.collision.indices.size() + static_cast<std::size_t>(faceCount) * 3);
            for (std::uint32_t i = 0; i < faceCount; ++i)
            {
                model.collision.indices.push_back(static_cast<std::uint32_t>(vertexBase + reader.u16()));
                model.collision.indices.push_back(static_cast<std::uint32_t>(vertexBase + reader.u16()));
                model.collision.indices.push_back(static_cast<std::uint32_t>(vertexBase + reader.u16()));
            }
            model.hasCollision = true;
            return reader.ok;
        }

        bool read_node(Reader& reader, DgModel& model, std::uint32_t depth)
        {
            if (depth > 64)
                return false;

            skip_vec3(reader); // Center.
            skip_bbox(reader); // View box.
            skip_bbox(reader); // Collision box.

            const auto meshGroupCount = reader.u32();
            if (!valid_count(meshGroupCount, 4096))
                return false;

            for (std::uint32_t groupIndex = 0; groupIndex < meshGroupCount; ++groupIndex)
            {
                const auto textureIndex = reader.u32();
                const auto meshCount = reader.u32();
                if (!valid_count(meshCount, 4096))
                    return false;

                for (std::uint32_t meshIndex = 0; meshIndex < meshCount; ++meshIndex)
                {
                    DgMesh mesh{};
                    if (!read_mesh(reader, textureIndex, mesh))
                        return false;
                    if (mesh.textureIndex < model.textures.size())
                        mesh.textureName = model.textures[mesh.textureIndex];
                    model.meshes.push_back(std::move(mesh));
                }
            }

            const auto collisionType = reader.u32();
            if (collisionType == 1 && !read_collision_mesh(reader, model))
                return false;

            for (std::uint32_t child = 0; child < 8; ++child)
            {
                const auto hasChild = reader.u32();
                if (hasChild > 0 && !read_node(reader, model, depth + 1))
                    return false;
            }

            return reader.ok;
        }
    }

    DgModel load_dg(const std::filesystem::path& path)
    {
        DgModel model{};

        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return model;

        const auto fileSize = stream.tellg();
        stream.seekg(0, std::ios::beg);

        std::vector<std::uint8_t> data(static_cast<std::size_t>(fileSize));
        stream.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!stream || data.size() < 36)
            return model;

        Reader reader{ data };
        read_bbox(reader, model.center, model.extent);

        const auto textureCount = reader.u32();
        if (!valid_count(textureCount, 4096))
            return model;

        model.textures.reserve(textureCount);
        for (std::uint32_t i = 0; i < textureCount; ++i)
            model.textures.push_back(reader.string256());

        const auto lightmapCount = reader.u32();
        (void)lightmapCount;
        const auto hasRoot = reader.u32();

        if (hasRoot > 0)
            model.parsed = read_node(reader, model, 0);

        {
            std::ofstream log(phoenix::core::engine_log_path(), std::ios::app);
            log << "DG: " << path.filename().string()
                << " textures=" << textureCount
                << " meshes=" << model.meshes.size()
                << " parsed=" << (model.parsed && !model.meshes.empty())
                << "\n";
        }

        model.parsed = model.parsed && !model.meshes.empty();
        return model;
    }
}
