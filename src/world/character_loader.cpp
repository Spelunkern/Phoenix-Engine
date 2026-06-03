#include "world/character_loader.h"

#include "assets/data_index.h"

#include <bit>
#include <fstream>
#include <string_view>

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

        std::int32_t read_i32(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            return std::bit_cast<std::int32_t>(read_u32(data, offset));
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

        bool read_file(const std::filesystem::path& path, std::vector<std::uint8_t>& data)
        {
            data = assets::read_file_binary(path);
            return !data.empty();
        }

        bool read_matrix(const std::vector<std::uint8_t>& data, std::size_t& offset, float (&matrix)[16])
        {
            if (offset + 64 > data.size())
                return false;
            // Shaiya serializes matrices column-first.
            matrix[0] = read_f32(data, offset + 0);
            matrix[4] = read_f32(data, offset + 4);
            matrix[8] = read_f32(data, offset + 8);
            matrix[12] = read_f32(data, offset + 12);
            matrix[1] = read_f32(data, offset + 16);
            matrix[5] = read_f32(data, offset + 20);
            matrix[9] = read_f32(data, offset + 24);
            matrix[13] = read_f32(data, offset + 28);
            matrix[2] = read_f32(data, offset + 32);
            matrix[6] = read_f32(data, offset + 36);
            matrix[10] = read_f32(data, offset + 40);
            matrix[14] = read_f32(data, offset + 44);
            matrix[3] = read_f32(data, offset + 48);
            matrix[7] = read_f32(data, offset + 52);
            matrix[11] = read_f32(data, offset + 56);
            matrix[15] = read_f32(data, offset + 60);
            offset += 64;
            return true;
        }
    }

    CharacterModel load_character_3dc(const std::filesystem::path& path)
    {
        CharacterModel model{};

        std::vector<std::uint8_t> data;
        if (!read_file(path, data) || data.size() < 4)
            return model;

        std::size_t offset = 0;
        const auto version = read_i32(data, offset);
        offset += 4;
        model.ep6 = version == 444;

        if (offset + 4 > data.size())
            return {};

        const auto boneCount = read_u32(data, offset);
        offset += 4;
        if (boneCount > 4096)
            return {};

        model.bones.reserve(boneCount);
        for (std::uint32_t i = 0; i < boneCount; ++i)
        {
            CharacterBone bone{};
            if (!read_matrix(data, offset, bone.matrix))
                return {};
            model.bones.push_back(bone);
        }

        if (offset + 4 > data.size())
            return {};

        const auto vertexCount = read_u32(data, offset);
        offset += 4;
        if (vertexCount > 1'000'000)
            return {};

        const auto vertexBytes = model.ep6 ? 48u : 40u;
        if (offset + static_cast<std::size_t>(vertexCount) * vertexBytes > data.size())
            return {};

        model.vertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i)
        {
            CharacterVertex vertex{};
            vertex.position[0] = read_f32(data, offset);
            vertex.position[1] = read_f32(data, offset + 4);
            vertex.position[2] = read_f32(data, offset + 8);
            offset += 12;

            vertex.boneWeights[0] = read_f32(data, offset);
            offset += 4;
            if (model.ep6)
            {
                vertex.boneWeights[1] = read_f32(data, offset);
                vertex.boneWeights[2] = read_f32(data, offset + 4);
                offset += 8;
            }
            else
            {
                vertex.boneWeights[1] = 1.0f - vertex.boneWeights[0];
            }

            vertex.boneIndices[0] = data[offset++];
            vertex.boneIndices[1] = data[offset++];
            vertex.boneIndices[2] = data[offset++];
            ++offset; // Unknown byte.

            vertex.normal[0] = read_f32(data, offset);
            vertex.normal[1] = read_f32(data, offset + 4);
            vertex.normal[2] = read_f32(data, offset + 8);
            offset += 12;

            vertex.uv[0] = read_f32(data, offset);
            vertex.uv[1] = read_f32(data, offset + 4);
            offset += 8;
            model.vertices.push_back(vertex);
        }

        if (offset + 4 > data.size())
            return {};

        const auto faceCount = read_u32(data, offset);
        offset += 4;
        if (faceCount > 1'000'000 || offset + static_cast<std::size_t>(faceCount) * 6 > data.size())
            return {};

        model.faces.reserve(faceCount);
        for (std::uint32_t i = 0; i < faceCount; ++i)
        {
            CharacterFace face{};
            face.indices[0] = read_u16(data, offset);
            face.indices[1] = read_u16(data, offset + 2);
            face.indices[2] = read_u16(data, offset + 4);
            offset += 6;
            model.faces.push_back(face);
        }

        model.parsed = true;
        return model;
    }

    CharacterModel load_cloak_3dc(const std::filesystem::path& path)
    {
        CharacterModel model{};

        std::vector<std::uint8_t> data;
        if (!read_file(path, data) || data.size() < 8)
            return model;

        std::size_t offset = 0;

        // Cloak 3DC: no version field. First uint32 = boneCount (always 0).
        const auto boneCount = read_u32(data, offset);
        offset += 4;
        if (boneCount != 0)
            return model; // Not a boneless cloak mesh.

        const auto vertexCount = read_u32(data, offset);
        offset += 4;
        if (vertexCount == 0 || vertexCount > 1'000'000)
            return model;

        // Vertices are 40 bytes: position(12) + weight(4) + boneIdx+pad(4) + normal(12) + uv(8).
        constexpr std::size_t kVertexBytes = 40;
        if (offset + static_cast<std::size_t>(vertexCount) * kVertexBytes + 4 > data.size())
            return model;

        model.vertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i)
        {
            CharacterVertex v{};
            v.position[0] = read_f32(data, offset);
            v.position[1] = read_f32(data, offset + 4);
            v.position[2] = read_f32(data, offset + 8);
            offset += 12;
            // weight(4) + boneIdx(1) + unknown(3) = 8 bytes — skip.
            offset += 8;
            v.normal[0] = read_f32(data, offset);
            v.normal[1] = read_f32(data, offset + 4);
            v.normal[2] = read_f32(data, offset + 8);
            offset += 12;
            v.uv[0] = read_f32(data, offset);
            v.uv[1] = read_f32(data, offset + 4);
            offset += 8;
            model.vertices.push_back(v);
        }

        if (offset + 4 > data.size())
            return model;

        const auto faceCount = read_u32(data, offset);
        offset += 4;
        if (faceCount > 1'000'000 || offset + static_cast<std::size_t>(faceCount) * 6 > data.size())
            return model;

        model.faces.reserve(faceCount);
        for (std::uint32_t i = 0; i < faceCount; ++i)
        {
            CharacterFace face{};
            face.indices[0] = read_u16(data, offset);
            face.indices[1] = read_u16(data, offset + 2);
            face.indices[2] = read_u16(data, offset + 4);
            offset += 6;
            model.faces.push_back(face);
        }

        model.parsed = true;
        return model;
    }

    CharacterAnimation load_character_ani(const std::filesystem::path& path)
    {
        CharacterAnimation animation{};

        std::vector<std::uint8_t> data;
        if (!read_file(path, data) || data.size() < 10)
            return animation;

        std::size_t offset = 0;
        if (data.size() >= 6 && std::string_view(reinterpret_cast<const char*>(data.data()), 6) == "ANI_V2")
        {
            animation.ep6 = true;
            offset += 6;
        }

        if (offset + 10 > data.size())
            return {};

        animation.startKeyframe = read_u32(data, offset);
        animation.endKeyframe = read_u32(data, offset + 4);
        offset += 8;

        const auto boneCount = read_u16(data, offset);
        offset += 2;
        if (boneCount > 4096)
            return {};

        animation.bones.reserve(boneCount);
        for (std::uint16_t i = 0; i < boneCount; ++i)
        {
            if (offset + 4 > data.size())
                return {};

            CharacterAnimationBone bone{};
            bone.parentBoneIndex = read_i32(data, offset);
            offset += 4;
            if (!read_matrix(data, offset, bone.matrix) || offset + 4 > data.size())
                return {};

            const auto rotationCount = read_u32(data, offset);
            offset += 4;
            if (rotationCount > 100000 || offset + static_cast<std::size_t>(rotationCount) * 20 > data.size())
                return {};

            bone.rotationFrames.reserve(rotationCount);
            for (std::uint32_t frameIndex = 0; frameIndex < rotationCount; ++frameIndex)
            {
                CharacterAnimationRotationFrame frame{};
                frame.frame = read_u32(data, offset);
                frame.quaternion[0] = read_f32(data, offset + 4);
                frame.quaternion[1] = read_f32(data, offset + 8);
                frame.quaternion[2] = read_f32(data, offset + 12);
                frame.quaternion[3] = read_f32(data, offset + 16);
                offset += 20;
                bone.rotationFrames.push_back(frame);
            }

            if (offset + 4 > data.size())
                return {};

            const auto translationCount = read_u32(data, offset);
            offset += 4;
            if (translationCount > 100000 || offset + static_cast<std::size_t>(translationCount) * 16 > data.size())
                return {};

            bone.translationFrames.reserve(translationCount);
            for (std::uint32_t frameIndex = 0; frameIndex < translationCount; ++frameIndex)
            {
                CharacterAnimationTranslationFrame frame{};
                frame.frame = read_u32(data, offset);
                frame.translation[0] = read_f32(data, offset + 4);
                frame.translation[1] = read_f32(data, offset + 8);
                frame.translation[2] = read_f32(data, offset + 12);
                offset += 16;
                bone.translationFrames.push_back(frame);
            }

            animation.bones.push_back(std::move(bone));
        }

        animation.parsed = true;
        return animation;
    }

    ItemModel load_item_3do(const std::filesystem::path& path)
    {
        ItemModel model{};

        std::vector<std::uint8_t> data;
        if (!read_file(path, data) || data.size() < 4)
            return model;

        std::size_t offset = 0;

        // Texture name: int32 length + char[length].
        const auto nameLen = read_u32(data, offset);
        offset += 4;
        if (nameLen > 256 || offset + nameLen > data.size())
            return {};
        model.textureName.assign(reinterpret_cast<const char*>(data.data() + offset), nameLen);
        offset += nameLen;

        // Vertices: int32 count + vertex data (3 float pos + 3 float normal + 2 float uv = 32 bytes).
        if (offset + 4 > data.size())
            return {};
        const auto vertexCount = read_u32(data, offset);
        offset += 4;
        if (vertexCount > 1'000'000 || offset + static_cast<std::size_t>(vertexCount) * 32 > data.size())
            return {};

        model.vertices.reserve(vertexCount);
        for (std::uint32_t i = 0; i < vertexCount; ++i)
        {
            ItemVertex v{};
            v.position[0] = read_f32(data, offset);
            v.position[1] = read_f32(data, offset + 4);
            v.position[2] = read_f32(data, offset + 8);
            v.normal[0] = read_f32(data, offset + 12);
            v.normal[1] = read_f32(data, offset + 16);
            v.normal[2] = read_f32(data, offset + 20);
            v.uv[0] = read_f32(data, offset + 24);
            v.uv[1] = read_f32(data, offset + 28);
            offset += 32;
            model.vertices.push_back(v);
        }

        // Faces: int32 count + 3×uint16 per face.
        if (offset + 4 > data.size())
            return {};
        const auto faceCount = read_u32(data, offset);
        offset += 4;
        if (faceCount > 1'000'000 || offset + static_cast<std::size_t>(faceCount) * 6 > data.size())
            return {};

        model.faces.reserve(faceCount);
        for (std::uint32_t i = 0; i < faceCount; ++i)
        {
            CharacterFace face{};
            face.indices[0] = read_u16(data, offset);
            face.indices[1] = read_u16(data, offset + 2);
            face.indices[2] = read_u16(data, offset + 4);
            offset += 6;
            model.faces.push_back(face);
        }

        model.parsed = true;
        return model;
    }
}
