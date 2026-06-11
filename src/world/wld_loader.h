#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace phoenix::world
{
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
        float position[3]{};
        float rotationForward[3]{};
        float rotationUp[3]{};
    };

    struct WldEffectInstance
    {
        float position[3]{};
        float rotationForward[3]{};
        float rotationUp[3]{};
        std::int32_t effectId{};
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
    };

    struct WldSoundEffect
    {
        std::int32_t soundEffectAssetId{};
        float center[3]{};
        float radius{};
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
        float fogColor[3]{ 0.42f, 0.58f, 0.74f };
        float fogStartDistance{ 800.0f };
        float fogEndDistance{ 4200.0f };
        std::string effectFileName;
        std::vector<WldEffectInstance> effectInstances;
        std::vector<std::string> musicAssets;
        std::vector<WldMusicZone> musicZones;
        std::vector<std::string> soundEffectAssets;
        std::vector<WldSoundEffect> soundEffects;
        std::vector<WldPortal> portals;
        std::vector<WldObjectSection> objectSections;
        std::vector<std::string> maniAssets;
        std::vector<WldManiInstance> maniInstances;
        std::string dungeonDgFileName;
        bool isDungeon{};
        bool parsed{};
        bool parsedSky{};
        // Field lightmaps directory for this map (World/field/<id>/).
        std::filesystem::path phoenixWorldFieldDir;
    };

    WldAnalysis analyze_wld(const std::filesystem::path& path);
}
