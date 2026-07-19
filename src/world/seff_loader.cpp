#include "world/seff_loader.h"

#include "assets/data_index.h"
#include "world/eft_binary_reader.h"

namespace phoenix::world
{
    namespace
    {
        using phoenix::world::detail::EftReader;

        SeffEffect read_seff_effect(EftReader& reader, std::int32_t format)
        {
            SeffEffect effect{};
            effect.particleCount = reader.u32();
            effect.particleVelocity = reader.f32();
            effect.textureBlendMode = reader.u32();
            effect.startPositionMultiplier = reader.u32();
            effect.particleLifetimeMs = reader.u32();
            effect.unknown6 = reader.f32();
            effect.textureFileName = reader.stringUtf16(512);
            effect.red = reader.u8();
            effect.green = reader.u8();
            effect.blue = reader.u8();
            reader.vec3(effect.particleStartPosition);
            effect.unknown10 = reader.f32();
            effect.particleStartSize = reader.f32();
            effect.isVisible = reader.u8() != 0;

            effect.unknown12 = reader.f32();

            if (format > 2)
                effect.rotateWithStretchMultiplier = reader.f32();
            if (format > 3)
                effect.velocityMultiplier = reader.f32();
            if (format > 5)
                effect.unknown15 = reader.u32();

            return effect;
        }

        SeffRecord read_seff_record(EftReader& reader, std::int32_t format)
        {
            SeffRecord record{};
            record.id = reader.i32();

            const auto effectCount = reader.count(256);
            record.effects.reserve(effectCount);
            for (std::uint32_t i = 0; i < effectCount && reader.ok; ++i)
                record.effects.push_back(read_seff_effect(reader, format));

            return record;
        }
    }

    SeffLibrary load_seff(const std::filesystem::path& path)
    {
        SeffLibrary library{};

        const auto data = phoenix::assets::read_file_binary(path);
        if (data.size() < 4)
            return library;

        EftReader reader{ data };

        library.format = reader.i32();

        library.timeStamp.year = static_cast<std::int16_t>(reader.u16());
        library.timeStamp.month = static_cast<std::int16_t>(reader.u16());
        library.timeStamp.day = static_cast<std::int16_t>(reader.u16());
        library.timeStamp.hour = static_cast<std::int16_t>(reader.u16());
        library.timeStamp.minute = static_cast<std::int16_t>(reader.u16());
        library.timeStamp.second = static_cast<std::int16_t>(reader.u16());

        const auto recordCount = reader.count(256);
        library.records.reserve(recordCount);
        for (std::uint32_t i = 0; i < recordCount && reader.ok; ++i)
            library.records.push_back(read_seff_record(reader, library.format));

        library.parsed = reader.ok;
        if (!library.parsed)
            library.records.clear();

        return library;
    }
}
