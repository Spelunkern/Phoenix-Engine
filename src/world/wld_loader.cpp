#include "world/wld_loader.h"

#include "assets/data_index.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>
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

        std::string read_string256(const std::vector<std::uint8_t>& data, std::size_t offset)
        {
            if (offset + 256 > data.size())
                return {};

            std::size_t length = 0;
            while (length < 256 && data[offset + length] != 0)
                ++length;

            return std::string(
                reinterpret_cast<const char*>(data.data() + offset),
                length);
        }

        bool skip_bytes(const std::vector<std::uint8_t>& data, std::size_t& offset, std::size_t count)
        {
            if (offset > data.size() || count > data.size() - offset)
                return false;

            offset += count;
            return true;
        }

        bool skip_fixed_list(const std::vector<std::uint8_t>& data, std::size_t& offset, std::size_t recordSize)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            return skip_bytes(data, offset, static_cast<std::size_t>(count) * recordSize);
        }

        bool skip_names(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            return skip_fixed_list(data, offset, 256);
        }

        bool skip_names_and_object_instances(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            return skip_names(data, offset)
                && skip_fixed_list(data, offset, 40);
        }

        bool read_names(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<std::string>& names)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 256 > data.size())
                return false;

            names.reserve(names.size() + count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                names.push_back(read_string256(data, offset));
                offset += 256;
            }

            return true;
        }

        bool read_object_instances(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<WldObjectInstance>& instances)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 1000000 || offset + static_cast<std::size_t>(count) * 40 > data.size())
                return false;

            instances.reserve(instances.size() + count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldObjectInstance instance{};
                instance.assetIndex = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                for (float& value : instance.position)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationForward)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationUp)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }

                instances.push_back(instance);
            }

            return true;
        }

        bool read_names_and_object_instances(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::string name,
            WldAnalysis& analysis)
        {
            WldObjectSection section{};
            section.name = std::move(name);
            if (!read_names(data, offset, section.assets)
                || !read_object_instances(data, offset, section.instances))
                return false;

            analysis.objectSections.push_back(std::move(section));
            return true;
        }

        bool read_mani_instances(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<WldManiInstance>& instances)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 44 > data.size())
                return false;

            instances.reserve(instances.size() + count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldManiInstance instance{};
                instance.buildingAssetId = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                instance.maniAssetIndex = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                for (float& value : instance.position)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationForward)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationUp)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                instances.push_back(instance);
            }
            return true;
        }

        bool read_effect_instances(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<WldEffectInstance>& instances)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 40 > data.size())
                return false;

            instances.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldEffectInstance instance{};
                for (float& value : instance.position)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationForward)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                for (float& value : instance.rotationUp)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                instance.effectId = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                instances.push_back(instance);
            }

            return true;
        }

        bool read_bounding_box(const std::vector<std::uint8_t>& data, std::size_t& offset, WldBoundingBox& box)
        {
            if (offset + 24 > data.size())
                return false;

            for (float& value : box.min)
            {
                value = read_f32(data, offset);
                offset += 4;
            }
            for (float& value : box.max)
            {
                value = read_f32(data, offset);
                offset += 4;
            }
            return true;
        }

        bool read_music_zones(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<WldMusicZone>& zones)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 36 > data.size())
                return false;

            zones.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldMusicZone zone{};
                if (!read_bounding_box(data, offset, zone.box))
                    return false;
                zone.radius = read_f32(data, offset);
                offset += 4;
                zone.musicAssetId = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                zone.unknown = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                zones.push_back(zone);
            }
            return true;
        }

        bool read_sound_effects(
            const std::vector<std::uint8_t>& data,
            std::size_t& offset,
            std::vector<WldSoundEffect>& sounds)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 20 > data.size())
                return false;

            sounds.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldSoundEffect sound{};
                sound.soundEffectAssetId = static_cast<std::int32_t>(read_u32(data, offset));
                offset += 4;
                for (float& value : sound.center)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                sound.radius = read_f32(data, offset);
                offset += 4;
                sounds.push_back(sound);
            }
            return true;
        }

        bool read_portals(const std::vector<std::uint8_t>& data, std::size_t& offset, std::vector<WldPortal>& portals)
        {
            if (offset + 4 > data.size())
                return false;

            const auto count = read_u32(data, offset);
            offset += 4;
            if (count > 100000 || offset + static_cast<std::size_t>(count) * 556 > data.size())
                return false;

            portals.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
            {
                WldPortal portal{};
                if (!read_bounding_box(data, offset, portal.box))
                    return false;
                portal.radius = read_f32(data, offset);
                offset += 4;
                portal.text1 = read_string256(data, offset);
                offset += 256;
                portal.text2 = read_string256(data, offset);
                offset += 256;
                portal.mapId = data[offset++];
                portal.faction = static_cast<std::int16_t>(read_u16(data, offset));
                offset += 2;
                portal.unknown = data[offset++];
                for (float& value : portal.destinationPosition)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                }
                portals.push_back(std::move(portal));
            }
            return true;
        }

        bool skip_zones(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            if (offset + 4 > data.size())
                return false;

            const auto zoneCount = read_u32(data, offset);
            offset += 4;
            for (std::uint32_t i = 0; i < zoneCount; ++i)
            {
                if (!skip_bytes(data, offset, 24))
                    return false;

                if (offset + 4 > data.size())
                    return false;

                const auto identifierCount = read_u32(data, offset);
                offset += 4;
                if (!skip_bytes(data, offset, static_cast<std::size_t>(identifierCount) * 4))
                    return false;
            }

            return true;
        }

        bool skip_npcs(const std::vector<std::uint8_t>& data, std::size_t& offset)
        {
            if (offset + 4 > data.size())
                return false;

            auto npcRecordCounter = static_cast<std::int32_t>(read_u32(data, offset));
            offset += 4;
            while (npcRecordCounter > 0)
            {
                if (!skip_bytes(data, offset, 24))
                    return false;

                if (offset + 4 > data.size())
                    return false;

                const auto patrolCount = read_u32(data, offset);
                offset += 4;
                if (!skip_bytes(data, offset, static_cast<std::size_t>(patrolCount) * 12))
                    return false;

                npcRecordCounter -= static_cast<std::int32_t>(patrolCount);
                --npcRecordCounter;
            }

            return true;
        }

        bool parse_field_tail(const std::vector<std::uint8_t>& data, std::size_t offset, WldAnalysis& analysis)
        {
            if (offset + 256 > data.size())
                return false;

            analysis.layoutName = read_string256(data, offset);
            offset += 256;

            if (!read_names_and_object_instances(data, offset, "Building", analysis)
                || !read_names_and_object_instances(data, offset, "Shape", analysis)
                || !read_names_and_object_instances(data, offset, "Tree", analysis)
                || !read_names_and_object_instances(data, offset, "Grass", analysis)
                || !read_names_and_object_instances(data, offset, "PrimaryVani", analysis)
                || !read_names_and_object_instances(data, offset, "SecondaryVani", analysis)
                || !read_names_and_object_instances(data, offset, "Dungeon", analysis))
                return false;

            // MAni assets and instances.
            if (!read_names(data, offset, analysis.maniAssets)
                || !read_mani_instances(data, offset, analysis.maniInstances)
                || offset + 256 > data.size())
                return false;

            analysis.effectFileName = read_string256(data, offset);
            offset += 256;

            if (!read_effect_instances(data, offset, analysis.effectInstances)
                || !skip_bytes(data, offset, 12)
                || !read_names_and_object_instances(data, offset, "Object", analysis)
                || !read_names(data, offset, analysis.musicAssets)
                || !read_music_zones(data, offset, analysis.musicZones)
                || !read_names(data, offset, analysis.soundEffectAssets)
                || !skip_zones(data, offset)
                || !read_sound_effects(data, offset, analysis.soundEffects)
                || !skip_fixed_list(data, offset, 28)
                || !read_portals(data, offset, analysis.portals)
                || !skip_fixed_list(data, offset, 40)
                || !skip_fixed_list(data, offset, 548)
                || !skip_npcs(data, offset))
                return false;

            if (offset + 768 > data.size())
                return false;

            analysis.skyFileName = read_string256(data, offset);
            offset += 256;
            analysis.primaryCloudFileName = read_string256(data, offset);
            offset += 256;
            analysis.secondaryCloudFileName = read_string256(data, offset);
            offset += 256;

            if (offset + 44 <= data.size())
            {
                // The WLD stores two unused colors before the fog color and distances.
                offset += 24;
                for (float& value : analysis.fogColor)
                {
                    value = read_f32(data, offset);
                    offset += 4;
                    if (value > 1.0f)
                        value /= 255.0f;
                }
                analysis.fogStartDistance = read_f32(data, offset);
                offset += 4;
                analysis.fogEndDistance = read_f32(data, offset);
            }

            analysis.parsedSky = true;
            return true;
        }
    }

    WldAnalysis analyze_wld(const std::filesystem::path& path)
    {
        WldAnalysis analysis{};
        analysis.path = path;

        auto data = assets::read_file_binary(path);
        if (data.size() < 12)
            return analysis;

        analysis.magic.assign(reinterpret_cast<const char*>(data.data()), 3);
        if (analysis.magic != "FLD" && analysis.magic != "DUN")
            return analysis;

        if (analysis.magic == "DUN")
        {
            analysis.isDungeon = true;
            if (data.size() < 260)
                return analysis;

            analysis.dungeonDgFileName = read_string256(data, 4);

            std::size_t offset = 260;
            if (!read_names_and_object_instances(data, offset, "Building", analysis)
                || !read_names_and_object_instances(data, offset, "Shape", analysis)
                || !read_names_and_object_instances(data, offset, "Tree", analysis)
                || !read_names_and_object_instances(data, offset, "Grass", analysis)
                || !read_names_and_object_instances(data, offset, "PrimaryVani", analysis)
                || !read_names_and_object_instances(data, offset, "SecondaryVani", analysis)
                || !read_names_and_object_instances(data, offset, "Dungeon", analysis))
            {
                analysis.parsed = !analysis.objectSections.empty();
                return analysis;
            }

            if (!skip_names(data, offset)
                || !skip_fixed_list(data, offset, 44)
                || offset + 256 > data.size())
            {
                analysis.parsed = !analysis.objectSections.empty();
                return analysis;
            }

            analysis.effectFileName = read_string256(data, offset);
            offset += 256;

            read_effect_instances(data, offset, analysis.effectInstances);
            if (offset + 12 <= data.size())
                offset += 12;
            read_names_and_object_instances(data, offset, "Object", analysis);
            read_names(data, offset, analysis.musicAssets);
            read_music_zones(data, offset, analysis.musicZones);
            read_names(data, offset, analysis.soundEffectAssets);
            skip_zones(data, offset);
            read_sound_effects(data, offset, analysis.soundEffects);
            skip_fixed_list(data, offset, 28);
            read_portals(data, offset, analysis.portals);

            analysis.parsed = true;
            return analysis;
        }

        analysis.mapSize = read_u32(data, 4);
        analysis.heightMapSide = (analysis.mapSize / 2) + 1;
        const auto sampleCount = analysis.heightMapSide * analysis.heightMapSide;
        const auto heightMapByteCount = static_cast<std::size_t>(sampleCount) * 2;
        const auto textureMapOffset = 8 + heightMapByteCount;
        const auto textureMapByteCount = static_cast<std::size_t>(sampleCount);
        const auto terrainLayerCountOffset = textureMapOffset + textureMapByteCount;
        if (data.size() < terrainLayerCountOffset + 4)
            return analysis;

        analysis.minInitialHeight = std::numeric_limits<float>::max();
        analysis.maxInitialHeight = std::numeric_limits<float>::lowest();
        analysis.heightSamples.reserve(sampleCount);

        for (std::uint32_t i = 0; i < sampleCount; ++i)
        {
            const auto value = static_cast<float>(read_u16(data, 8 + static_cast<std::size_t>(i) * 2));
            if (!std::isfinite(value))
                continue;

            if (i == 0)
                analysis.firstHeight = value;

            analysis.heightSamples.push_back(value);
            analysis.minInitialHeight = std::min(analysis.minInitialHeight, value);
            analysis.maxInitialHeight = std::max(analysis.maxInitialHeight, value);
        }

        analysis.terrainTextureMap.assign(
            data.begin() + static_cast<std::ptrdiff_t>(textureMapOffset),
            data.begin() + static_cast<std::ptrdiff_t>(textureMapOffset + textureMapByteCount));

        const auto terrainLayerCount = read_u32(data, terrainLayerCountOffset);
        constexpr std::uint32_t kReasonableTerrainLayerLimit = 256;
        if (terrainLayerCount <= kReasonableTerrainLayerLimit)
        {
            auto layerOffset = terrainLayerCountOffset + 4;
            for (std::uint32_t i = 0; i < terrainLayerCount; ++i)
            {
                if (layerOffset + 516 > data.size())
                    break;

                WldTerrainLayer layer{};
                layer.textureFileName = read_string256(data, layerOffset);
                layer.tileSize = read_f32(data, layerOffset + 256);
                layer.walkSoundFileName = read_string256(data, layerOffset + 260);
                analysis.terrainLayers.push_back(std::move(layer));
                layerOffset += 516;
            }
            parse_field_tail(data, layerOffset, analysis);
        }

        analysis.parsed = true;
        return analysis;
    }


}
