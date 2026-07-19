#include "world/eft_loader.h"

#include "assets/data_index.h"
#include "world/eft_binary_reader.h"

namespace phoenix::world
{
    namespace
    {
        using phoenix::world::detail::EftReader;

        EftEffect read_effect(EftReader& reader, EftFormat format)
        {
            EftEffect effect{};
            effect.name = reader.string(4096);

            effect.velocityRandomEnabled[0] = reader.i32() != 0;
            effect.velocityRandomEnabled[1] = reader.i32() != 0;
            effect.velocityRandomEnabled[2] = reader.i32() != 0;
            effect.loop = reader.i32() != 0;
            effect.destinationBlend = reader.i32();
            effect.velocityMode = reader.i32();
            effect.sourceBlend = reader.i32();
            effect.textureLoop = reader.i32() != 0;
            effect.meshIndex = reader.i32();
            effect.motionPathEnabled = reader.i32() != 0;

            effect.delayPerFrame = reader.f32();
            effect.emitRateMax = reader.f32();
            effect.lifeMax = reader.f32();
            effect.emitRateMin = reader.f32();
            effect.lifeMin = reader.f32();
            effect.emitterDuration = reader.f32();
            effect.swirlSpeed = reader.f32();
            effect.unknown18 = reader.f32();

            reader.vec3(effect.emitPositionSpread);
            reader.vec3(effect.acceleration);
            reader.vec3(effect.emitOrigin);
            reader.vec3(effect.velocityMin);
            reader.vec3(effect.velocityMax);

            effect.baseAxis = reader.i32();
            effect.gravityEnabled = reader.i32() != 0;
            effect.attractEnabled = reader.i32() != 0;
            reader.vec3(effect.attractPoint);
            effect.attractStrength = reader.f32();

            effect.angularVelocityRandom = reader.i32() != 0;
            effect.rotationEnabled = reader.i32() != 0;
            effect.angularVelocity = reader.f32();
            effect.rotationAxis = reader.i32();

            if (format == EftFormat::EF3)
            {
                reader.i32(); // Unused EF3 field.
                effect.distanceScaleMode = reader.i32();
            }

            const auto colorFrameCount = reader.count(100000);
            effect.colorFrames.reserve(colorFrameCount);
            for (std::uint32_t i = 0; i < colorFrameCount && reader.ok; ++i)
            {
                EftColorKeyframe frame{};
                frame.r = reader.f32();
                frame.g = reader.f32();
                frame.b = reader.f32();
                frame.a = reader.f32();
                frame.time = reader.f32();
                effect.colorFrames.push_back(frame);
            }

            const auto velocityScaleFrameCount = reader.count(100000);
            effect.velocityScaleFrames.reserve(velocityScaleFrameCount);
            for (std::uint32_t i = 0; i < velocityScaleFrameCount && reader.ok; ++i)
            {
                EftFloatKeyframe frame{};
                frame.value = reader.f32();
                frame.time = reader.f32();
                effect.velocityScaleFrames.push_back(frame);
            }

            const auto scaleFrameCount = reader.count(100000);
            effect.scaleFrames.reserve(scaleFrameCount);
            for (std::uint32_t i = 0; i < scaleFrameCount && reader.ok; ++i)
            {
                EftScaleKeyframe frame{};
                frame.min = reader.f32();
                frame.max = reader.f32();
                frame.time = reader.f32();
                effect.scaleFrames.push_back(frame);
            }

            effect.mirrorTexture = reader.i32() != 0;
            effect.initialRotationAxis = reader.i32();
            effect.initialRotationMinDegrees = reader.i32();
            effect.initialRotationMaxDegrees = reader.i32();

            const auto textureIdCount = reader.count(100000);
            effect.textureIds.reserve(textureIdCount);
            for (std::uint32_t i = 0; i < textureIdCount && reader.ok; ++i)
                effect.textureIds.push_back(reader.i32());

            return effect;
        }

        EftEffectSequence read_sequence(EftReader& reader)
        {
            EftEffectSequence sequence{};
            sequence.name = reader.string(4096);

            const auto recordCount = reader.count(100000);
            sequence.records.reserve(recordCount);
            for (std::uint32_t i = 0; i < recordCount && reader.ok; ++i)
            {
                EftEffectSequenceRecord record{};
                record.effectId = reader.i32();
                record.time = reader.f32();
                sequence.records.push_back(record);
            }

            return sequence;
        }
    }

    EftLibrary load_eft(const std::filesystem::path& path)
    {
        EftLibrary library{};

        const auto data = phoenix::assets::read_file_binary(path);
        if (data.size() < 3)
            return library;

        EftReader reader{ data };
        const std::string signature(reinterpret_cast<const char*>(data.data()), 3);
        reader.offset = 3;

        if (signature == "EFT")
            library.format = EftFormat::EFT;
        else if (signature == "EF2")
            library.format = EftFormat::EF2;
        else if (signature == "EF3")
            library.format = EftFormat::EF3;
        else
            return library;

        const auto meshCount = reader.count(256);
        library.meshNames.reserve(meshCount);
        for (std::uint32_t i = 0; i < meshCount && reader.ok; ++i)
            library.meshNames.push_back(reader.string(4096));

        const auto textureCount = reader.count(512);
        library.textureNames.reserve(textureCount);
        for (std::uint32_t i = 0; i < textureCount && reader.ok; ++i)
            library.textureNames.push_back(reader.string(4096));

        const auto effectCount = reader.count(1024);
        library.effects.reserve(effectCount);
        for (std::uint32_t i = 0; i < effectCount && reader.ok; ++i)
            library.effects.push_back(read_effect(reader, library.format));

        const auto sequenceCount = reader.count(256);
        library.sequences.reserve(sequenceCount);
        for (std::uint32_t i = 0; i < sequenceCount && reader.ok; ++i)
            library.sequences.push_back(read_sequence(reader));

        library.parsed = reader.ok;
        if (!library.parsed)
        {
            library.meshNames.clear();
            library.textureNames.clear();
            library.effects.clear();
            library.sequences.clear();
        }

        return library;
    }
}
