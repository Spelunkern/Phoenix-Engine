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

    // 3DO format — static mesh for weapons and shields (no bones).
    struct ItemVertex
    {
        float position[3]{};
        float normal[3]{};
        float uv[2]{};
    };

    struct ItemModel
    {
        std::string textureName;
        std::vector<ItemVertex> vertices;
        std::vector<CharacterFace> faces; // same uint16 triangle format
        bool parsed{};
    };

    CharacterModel load_character_3dc(const std::filesystem::path& path);
    // Cloak-specific 3DC loader: no version header, starts directly with boneCount=0.
    CharacterModel load_cloak_3dc(const std::filesystem::path& path);
    CharacterAnimation load_character_ani(const std::filesystem::path& path);
    ItemModel load_item_3do(const std::filesystem::path& path);
}
