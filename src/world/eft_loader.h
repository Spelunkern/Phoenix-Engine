#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    enum class EftFormat
    {
        Unknown,
        EFT,
        EF2,
        EF3,
    };

    struct EftColorKeyframe
    {
        float r{};
        float g{};
        float b{};
        float a{};
        float time{};
    };

    // Used for the "velocity scale over lifetime" keyframe track.
    struct EftFloatKeyframe
    {
        float value{};
        float time{};
    };

    // Particle scale is sampled per-particle as a random value between min and
    // max, then the whole track is interpolated over time like the other
    // keyframe lists.
    struct EftScaleKeyframe
    {
        float min{};
        float max{};
        float time{};
    };

    // One particle-emitter component of an .EFT effect. Field names/order follow
    // the effect-renderer reference parser (github.com/cupsocino/effect-renderer),
    // which validates them visually against the retail client; Parsec's C# reader
    // agrees on the binary layout but leaves several of these as "UnknownN".
    struct EftEffect
    {
        std::string name;
        bool velocityRandomEnabled[3]{};
        bool loop{};
        std::int32_t destinationBlend{};
        std::int32_t velocityMode{};
        std::int32_t sourceBlend{};
        bool textureLoop{};
        std::int32_t meshIndex{ -1 };
        bool motionPathEnabled{};
        float delayPerFrame{};
        float emitRateMax{};
        float lifeMax{};
        float emitRateMin{};
        float lifeMin{};
        float emitterDuration{};
        float swirlSpeed{};
        float unknown18{};
        float emitPositionSpread[3]{};
        float acceleration[3]{};
        float emitOrigin[3]{};
        float velocityMin[3]{};
        float velocityMax[3]{};
        std::int32_t baseAxis{};
        bool gravityEnabled{};
        bool attractEnabled{};
        float attractPoint[3]{};
        float attractStrength{};
        bool angularVelocityRandom{};
        bool rotationEnabled{};
        float angularVelocity{};
        std::int32_t rotationAxis{};
        // Only meaningful for EF3; zero otherwise.
        std::int32_t distanceScaleMode{};

        std::vector<EftColorKeyframe> colorFrames;
        std::vector<EftFloatKeyframe> velocityScaleFrames;
        std::vector<EftScaleKeyframe> scaleFrames;

        bool mirrorTexture{};
        std::int32_t initialRotationAxis{};
        std::int32_t initialRotationMinDegrees{};
        std::int32_t initialRotationMaxDegrees{};
        std::vector<std::int32_t> textureIds;
    };

    struct EftEffectSequenceRecord
    {
        std::int32_t effectId{};
        float time{};
    };

    struct EftEffectSequence
    {
        std::string name;
        std::vector<EftEffectSequenceRecord> records;
    };

    // A parsed .EFT/.EF2/.EF3 effect library (one file referenced by a map's
    // WldAnalysis::effectFileName, or by a monster/skill effect table).
    struct EftLibrary
    {
        EftFormat format{ EftFormat::Unknown };
        std::vector<std::string> meshNames;
        std::vector<std::string> textureNames;
        std::vector<EftEffect> effects;
        std::vector<EftEffectSequence> sequences;
        bool parsed{};
    };

    EftLibrary load_eft(const std::filesystem::path& path);
}
