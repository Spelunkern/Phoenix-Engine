#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
    struct WldStringReference
    {
        std::uint64_t offset{};
        std::string value;
    };

    struct WldTerrainLayer
    {
        std::string textureFileName;
        float tileSize{};
        std::string walkSoundFileName;
    };

    struct WldObjectInstance
    {
        std::int32_t assetIndex{};
        float position[3]{};
        float rotationForward[3]{};
        float rotationUp[3]{};
        std::uint64_t fileOffset{};
    };

    struct WldObjectSection
    {
        std::string name;
        std::vector<std::string> assets;
        std::vector<WldObjectInstance> instances;
    };

    struct WldManiInstance
    {
        std::int32_t buildingAssetId{};
        std::int32_t maniAssetIndex{};
        std::string buildingAssetName;
        float position[3]{};
        float rotationForward[3]{};
        float rotationUp[3]{};
        std::uint64_t fileOffset{};
    };

    struct WldEffectInstance
    {
        float position[3]{};
        float rotationForward[3]{};
        float rotationUp[3]{};
        std::int32_t effectId{};
        std::uint64_t fileOffset{};
    };

    struct WldBoundingBox
    {
        float min[3]{};
        float max[3]{};
    };

    struct WldMusicZone
    {
        WldBoundingBox box;
        float radius{};
        std::int32_t musicAssetId{};
        std::int32_t unknown{};
        std::uint64_t fileOffset{};
    };

    struct WldSoundEffect
    {
        std::int32_t soundEffectAssetId{};
        float center[3]{};
        float radius{};
        std::uint64_t fileOffset{};
    };

    struct WldPortal
    {
        WldBoundingBox box;
        float radius{};
        std::string text1;
        std::string text2;
        std::uint8_t mapId{};
        std::int16_t faction{};
        std::uint8_t unknown{};
        float destinationPosition[3]{};
        std::uint64_t fileOffset{};
    };

    struct WldAnalysis
    {
        std::filesystem::path path;
        std::string magic;
        std::uint32_t mapSize{};
        std::uint32_t heightMapSide{};
        float firstHeight{};
        float minInitialHeight{};
        float maxInitialHeight{};
        std::vector<float> heightSamples;
        std::vector<std::uint8_t> terrainTextureMap;
        std::vector<WldTerrainLayer> terrainLayers;
        std::string layoutName;
        std::string skyFileName;
        std::string primaryCloudFileName;
        std::string secondaryCloudFileName;
        std::uint64_t skyFileNameOffset{};
        std::uint64_t primaryCloudFileNameOffset{};
        std::uint64_t secondaryCloudFileNameOffset{};
        bool hasSkyOffsets{};
        float fogColor[3]{ 0.42f, 0.58f, 0.74f };
        float fogStartDistance{ 800.0f };
        float fogEndDistance{ 4200.0f };
        std::uint64_t fogColorOffset{};
        std::uint64_t fogStartOffset{};
        std::uint64_t fogEndOffset{};
        bool hasFogOffsets{};
        std::vector<std::uint64_t> terrainLayerTextureOffsets;
        bool hasTerrainLayerOffsets{};
        std::string effectFileName;
        std::vector<WldEffectInstance> effectInstances;
        std::vector<std::string> musicAssets;
        std::uint64_t musicAssetCountOffset{};
        std::uint32_t musicAssetOriginalCount{};
        std::uint64_t musicZoneCountOffset{};
        std::vector<WldMusicZone> musicZones;
        std::vector<std::string> soundEffectAssets;
        std::uint64_t soundEffectAssetCountOffset{};
        std::uint32_t soundEffectAssetOriginalCount{};
        std::uint64_t soundEffectCountOffset{};
        std::vector<WldSoundEffect> soundEffects;
        std::vector<WldPortal> portals;
        std::vector<WldObjectSection> objectSections;
        std::vector<std::string> maniAssets;
        std::vector<WldManiInstance> maniInstances;
        std::vector<WldStringReference> resourceStrings;
        std::string dungeonDgFileName;
        bool isDungeon{};
        bool parsed{};
        bool parsedSky{};
        bool wrotePreview{};
        // When loaded from Phoenix Worlds, the self-contained world directory.
        std::filesystem::path phoenixWorldDir;
        // When loaded from Phoenix Worlds, field lightmaps live here.
        std::filesystem::path phoenixWorldFieldDir;
    };

    WldAnalysis analyze_wld(const std::filesystem::path& path, const std::filesystem::path& previewPath);
}
