#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

namespace phoenix::world
{
    struct CharacterVertex
    {
        float position[3]{};
        float normal[3]{};
        float uv[2]{};
        float boneWeights[3]{};
        std::uint8_t boneIndices[3]{};
    };

    struct CharacterFace
    {
        std::uint16_t indices[3]{};
    };

    struct CharacterBone
    {
        float matrix[16]{};
    };

    struct CharacterModel
    {
        std::vector<CharacterBone> bones;
        std::vector<CharacterVertex> vertices;
        std::vector<CharacterFace> faces;
        bool parsed{};
        bool ep6{};
    };

    struct CharacterAnimationRotationFrame
    {
        std::uint32_t frame{};
        float quaternion[4]{};
    };

    struct CharacterAnimationTranslationFrame
    {
        std::uint32_t frame{};
        float translation[3]{};
    };

    struct CharacterAnimationBone
    {
        std::int32_t parentBoneIndex{};
        float matrix[16]{};
        std::vector<CharacterAnimationRotationFrame> rotationFrames;
        std::vector<CharacterAnimationTranslationFrame> translationFrames;
    };

    struct CharacterAnimation
    {
        std::uint32_t startKeyframe{};
        std::uint32_t endKeyframe{};
        std::vector<CharacterAnimationBone> bones;
        bool parsed{};
        bool ep6{};
    };

    CharacterModel load_character_3dc(const std::filesystem::path& path);
    CharacterAnimation load_character_ani(const std::filesystem::path& path);
}
