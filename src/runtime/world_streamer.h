#pragma once

#include "runtime/phoenix_runtime.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phoenix::app { class LoadingScheduler; }
namespace phoenix::renderer { class OpenGLRenderer; }

namespace phoenix::runtime
{
    // Bounded residency controller for field props. Model parsing, DDS decode
    // and BC3 conversion run on loading workers; the render thread only
    // integrates one completed payload at a time and updates fixed GPU pools.
    class WorldStreamer
    {
    public:
        struct UpdateResult
        {
            bool sceneChanged{};
            std::size_t residentAssets{};
            std::size_t pendingAssets{};
        };

        ~WorldStreamer();

        void configure(
            PhoenixRuntime& runtime,
            phoenix::renderer::OpenGLRenderer& renderer,
            phoenix::app::LoadingScheduler& scheduler,
            std::uint32_t firstTextureLayer,
            std::uint32_t textureLayerCapacity,
            const std::vector<std::filesystem::path>& pinnedTexturePaths);

        UpdateResult update(float cameraX, float cameraZ, float viewDistance,
            std::chrono::microseconds integrationBudget = std::chrono::microseconds(4000));

        // Waits for the at-most-two in-flight jobs before their runtime source
        // catalog is destroyed during a map transition.
        void reset();
        bool active() const { return runtime_ != nullptr; }

    private:
        struct TextureLease
        {
            std::uint32_t layer{};
            std::uint32_t references{};
            bool pinned{};
            // Pinned paths already occupy a stable primary-array slot, but the
            // independent streamed-asset array still needs its own upload.
            bool uploaded{};
        };
        struct PendingAsset
        {
            std::size_t slot{};
            std::future<PhoenixRuntime::WorldAssetPayload> future;
        };
        struct StagedAsset
        {
            PhoenixRuntime::WorldAssetPayload payload;
            std::vector<std::uint32_t> layers;
            std::vector<bool> uploadNeeded;
            std::size_t nextTexture{};
        };

        std::vector<std::uint32_t> acquire_texture_layers(
            PhoenixRuntime::WorldAssetPayload& payload,
            std::vector<bool>& uploadNeeded);
        void release_texture_layers(std::size_t assetSlot);
        bool integrate_ready(std::chrono::steady_clock::time_point deadline);
        void launch_requests(const PhoenixRuntime::WorldStreamingDemand& demand);

        PhoenixRuntime* runtime_{};
        phoenix::renderer::OpenGLRenderer* renderer_{};
        phoenix::app::LoadingScheduler* scheduler_{};
        std::uint32_t firstTextureLayer_{};
        std::uint32_t textureLayerCapacity_{};
        std::uint32_t textureWidth_{};
        std::uint32_t textureHeight_{};
        std::uint32_t textureMipLevels_{};
        std::vector<std::uint32_t> freeTextureLayers_;
        std::unordered_map<std::string, TextureLease> textureLeases_;
        std::vector<std::vector<std::string>> assetTextureKeys_;
        std::vector<PendingAsset> pending_;
        std::optional<StagedAsset> staged_;
        std::unordered_set<std::size_t> queuedSlots_;
        std::unordered_set<std::size_t> failedSlots_;
    };
}
