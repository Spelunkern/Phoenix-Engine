#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct EftColorFrame
    {
        float r{}, g{}, b{}, a{};
        float time{};
    };

    struct EftOpacityFrame
    {
        float opacity{};
        float time{};
    };

    struct EftScaleFrame
    {
        float x{}, y{};
        float time{};
    };

    struct EftEffect
    {
        std::string name;
        std::int32_t meshIndex{ -1 };
        std::int32_t srcBlend{};
        std::int32_t destBlend{};
        float position[3]{};
        float delayPerFrame{};
        float initialDelay{};
        bool rotationEnabled{};
        std::int32_t rotationAxis{};
        float rotationSpeedMin{};
        float rotationSpeedMax{};
        std::vector<EftColorFrame> colorFrames;
        std::vector<EftOpacityFrame> opacityFrames;
        std::vector<EftScaleFrame> scaleFrames;
        std::vector<std::int32_t> textureIndices;
    };

    struct EftSequenceRecord
    {
        std::int32_t effectId{};
        float time{};
    };

    struct EftSequence
    {
        std::string name;
        std::vector<EftSequenceRecord> records;
    };

    struct EftFile
    {
        std::string signature;
        std::vector<std::string> meshNames;
        std::vector<std::string> textureNames;
        std::vector<EftEffect> effects;
        std::vector<EftSequence> sequences;
        bool parsed{};
    };

    EftFile load_eft(const std::filesystem::path& path);
}
