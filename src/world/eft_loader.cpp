#include "world/eft_loader.h"

#include <cstring>
#include <fstream>

namespace phoenix::world
{
    namespace
    {
        struct EftReader
        {
            const std::uint8_t* data{};
            std::size_t size{};
            std::size_t offset{};

            bool ok() const { return offset <= size; }
            bool has(std::size_t bytes) const { return offset + bytes <= size; }

            std::int32_t i32()
            {
                if (!has(4)) { offset = size + 1; return 0; }
                std::int32_t v{};
                std::memcpy(&v, data + offset, 4);
                offset += 4;
                return v;
            }

            float f32()
            {
                if (!has(4)) { offset = size + 1; return 0.0f; }
                float v{};
                std::memcpy(&v, data + offset, 4);
                offset += 4;
                return v;
            }

            std::string str()
            {
                const auto length = i32();
                if (length <= 0 || length > 4096 || !has(static_cast<std::size_t>(length)))
                {
                    if (length > 0) offset = size + 1;
                    return {};
                }
                // Find null terminator.
                std::size_t end = 0;
                while (end < static_cast<std::size_t>(length) && data[offset + end] != 0) ++end;
                std::string result(reinterpret_cast<const char*>(data + offset), end);
                offset += static_cast<std::size_t>(length);
                return result;
            }

            void skip(std::size_t bytes)
            {
                if (!has(bytes)) { offset = size + 1; return; }
                offset += bytes;
            }
        };

        EftEffect read_effect(EftReader& r, const std::string& signature)
        {
            EftEffect effect;
            effect.name = r.str();

            // unknown1, unknown2, unknown3
            r.i32(); r.i32(); r.i32();
            // loop
            r.i32();
            // srcBlend
            effect.srcBlend = r.i32();
            // unknown6
            r.i32();
            // destBlend
            effect.destBlend = r.i32();
            // unknown8
            r.i32();
            // meshIndex
            effect.meshIndex = r.i32();
            // unknown10
            r.i32();
            // delayPerFrame
            effect.delayPerFrame = r.f32();
            // unknown12, unknown13, unknown14
            r.f32(); r.f32(); r.f32();
            // initialDelay
            effect.initialDelay = r.f32();
            // unknown16, unknown17, unknown18
            r.f32(); r.f32(); r.f32();
            // offsetFrame (vec3)
            r.f32(); r.f32(); r.f32();
            // trembling (vec3)
            r.f32(); r.f32(); r.f32();
            // position (vec3)
            effect.position[0] = r.f32();
            effect.position[1] = r.f32();
            effect.position[2] = r.f32();
            // spread1 (vec3)
            r.f32(); r.f32(); r.f32();
            // spread2 (vec3)
            r.f32(); r.f32(); r.f32();
            // baseAxis
            r.i32();
            // unknown20, unknown21
            r.i32(); r.i32();
            // unknownVec6 (vec3)
            r.f32(); r.f32(); r.f32();
            // rotationSpeedMin
            effect.rotationSpeedMin = r.f32();
            // rotationRandomEnabled
            r.i32();
            // rotationEnabled
            effect.rotationEnabled = r.i32() != 0;
            // rotationSpeedMax
            effect.rotationSpeedMax = r.f32();
            // rotationAxis
            effect.rotationAxis = r.i32();

            // EF3 extra fields.
            if (signature == "EF3")
            {
                r.f32(); r.f32();
            }

            // colorFrames
            const auto colorCount = r.i32();
            if (colorCount > 0 && colorCount < 10000)
            {
                effect.colorFrames.resize(static_cast<std::size_t>(colorCount));
                for (auto& frame : effect.colorFrames)
                {
                    frame.r = r.f32();
                    frame.g = r.f32();
                    frame.b = r.f32();
                    frame.a = r.f32();
                    frame.time = r.f32();
                }
            }

            // opacityFrames
            const auto opacityCount = r.i32();
            for (std::int32_t i = 0; i < opacityCount && i < 10000; ++i)
            {
                EftOpacityFrame of;
                of.opacity = r.f32();
                of.time = r.f32();
                effect.opacityFrames.push_back(of);
            }

            // scaleFrames
            const auto scaleCount = r.i32();
            for (std::int32_t i = 0; i < scaleCount && i < 10000; ++i)
            {
                EftScaleFrame sf;
                sf.x = r.f32();
                sf.y = r.f32();
                sf.time = r.f32();
                effect.scaleFrames.push_back(sf);
            }

            // unknown29, unknown30, unknown31, unknown32
            r.i32(); r.i32(); r.i32(); r.i32();

            // texture references
            const auto texCount = r.i32();
            if (texCount > 0 && texCount < 10000)
            {
                effect.textureIndices.resize(static_cast<std::size_t>(texCount));
                for (auto& id : effect.textureIndices)
                    id = r.i32();
            }

            return effect;
        }
    }

    EftFile load_eft(const std::filesystem::path& path)
    {
        EftFile eft;

        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return eft;

        const auto fileSize = static_cast<std::size_t>(file.tellg());
        if (fileSize < 7) return eft;

        std::vector<std::uint8_t> buffer(fileSize);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(fileSize));

        EftReader r;
        r.data = buffer.data();
        r.size = fileSize;
        r.offset = 0;

        // Signature: 3 ASCII bytes.
        if (fileSize < 3) return eft;
        eft.signature.assign(reinterpret_cast<const char*>(r.data), 3);
        r.offset = 3;

        if (eft.signature != "EFT" && eft.signature != "EF2" && eft.signature != "EF3")
            return eft;

        // Mesh names.
        const auto meshCount = r.i32();
        if (meshCount >= 0 && meshCount < 10000)
        {
            eft.meshNames.reserve(static_cast<std::size_t>(meshCount));
            for (std::int32_t i = 0; i < meshCount && r.ok(); ++i)
                eft.meshNames.push_back(r.str());
        }

        // Texture names.
        const auto texCount = r.i32();
        if (texCount >= 0 && texCount < 10000)
        {
            eft.textureNames.reserve(static_cast<std::size_t>(texCount));
            for (std::int32_t i = 0; i < texCount && r.ok(); ++i)
                eft.textureNames.push_back(r.str());
        }

        // Effects.
        const auto effectCount = r.i32();
        if (effectCount >= 0 && effectCount < 10000)
        {
            eft.effects.reserve(static_cast<std::size_t>(effectCount));
            for (std::int32_t i = 0; i < effectCount && r.ok(); ++i)
                eft.effects.push_back(read_effect(r, eft.signature));
        }

        // Sequences.
        const auto seqCount = r.i32();
        if (seqCount >= 0 && seqCount < 10000)
        {
            eft.sequences.reserve(static_cast<std::size_t>(seqCount));
            for (std::int32_t i = 0; i < seqCount && r.ok(); ++i)
            {
                EftSequence seq;
                seq.name = r.str();
                const auto recordCount = r.i32();
                if (recordCount > 0 && recordCount < 10000)
                {
                    seq.records.resize(static_cast<std::size_t>(recordCount));
                    for (auto& record : seq.records)
                    {
                        record.effectId = r.i32();
                        record.time = r.f32();
                    }
                }
                eft.sequences.push_back(std::move(seq));
            }
        }

        eft.parsed = r.ok();
        return eft;
    }
}
