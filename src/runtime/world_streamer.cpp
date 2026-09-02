#include "runtime/world_streamer.h"

#include "app/loading_scheduler.h"
#include "assets/data_index.h"
#include "renderer/opengl_renderer.h"

#include <algorithm>
#include <chrono>

namespace phoenix::runtime
{
    namespace
    {
        constexpr std::size_t kMaxConcurrentLoads = 2;
        constexpr std::size_t kMaxIntegrationsPerFrame = 1;
    }

    WorldStreamer::~WorldStreamer()
    {
        reset();
    }

    void WorldStreamer::configure(
        PhoenixRuntime& runtime,
        phoenix::renderer::OpenGLRenderer& renderer,
        phoenix::app::LoadingScheduler& scheduler,
        std::uint32_t firstTextureLayer,
        std::uint32_t textureLayerCapacity,
        const std::vector<std::filesystem::path>& pinnedTexturePaths)
    {
        reset();
        if (!runtime.uses_world_asset_streaming() || textureLayerCapacity == 0)
            return;

        runtime_ = &runtime;
        renderer_ = &renderer;
        scheduler_ = &scheduler;
        firstTextureLayer_ = firstTextureLayer;
        textureLayerCapacity_ = textureLayerCapacity;
        textureWidth_ = renderer.asset_texture_width();
        textureHeight_ = renderer.asset_texture_height();
        textureMipLevels_ = renderer.asset_texture_mip_levels();
        if (textureWidth_ == 0 || textureHeight_ == 0 || textureMipLevels_ == 0)
        {
            reset();
            return;
        }
        assetTextureKeys_.resize(runtime.state().worldAssets.size());

        const auto pinnedCount = std::min<std::size_t>(pinnedTexturePaths.size(), textureLayerCapacity_);
        for (std::size_t i = 0; i < pinnedCount; ++i)
        {
            if (pinnedTexturePaths[i].empty())
                continue;
            const auto key = phoenix::assets::lower_ascii(pinnedTexturePaths[i].string());
            textureLeases_[key] = {
                firstTextureLayer_ + static_cast<std::uint32_t>(i), 1u, true, false
            };
        }
        for (std::uint32_t i = static_cast<std::uint32_t>(pinnedCount);
            i < textureLayerCapacity_; ++i)
            freeTextureLayers_.push_back(firstTextureLayer_ + i);
        // Allocate low layers first for easier diagnostics and stable captures.
        std::ranges::reverse(freeTextureLayers_);
    }

    std::vector<std::uint32_t> WorldStreamer::acquire_texture_layers(
        PhoenixRuntime::WorldAssetPayload& payload,
        std::vector<bool>& uploadNeeded)
    {
        std::vector<std::uint32_t> layers(payload.texturePaths.size(), UINT32_MAX);
        uploadNeeded.assign(payload.texturePaths.size(), false);
        if (!renderer_ || payload.slot >= assetTextureKeys_.size())
            return layers;

        auto& keys = assetTextureKeys_[payload.slot];
        keys.clear();
        keys.resize(payload.texturePaths.size());
        for (std::size_t i = 0; i < payload.texturePaths.size(); ++i)
        {
            const auto& path = payload.texturePaths[i];
            if (path.empty())
                continue;
            auto key = phoenix::assets::lower_ascii(path.string());
            if (const auto found = textureLeases_.find(key); found != textureLeases_.end())
            {
                found->second.references++;
                layers[i] = found->second.layer;
                keys[i] = std::move(key);
                uploadNeeded[i] = !found->second.uploaded
                    && i < payload.textures.size() && payload.textures[i].valid;
                continue;
            }
            if (freeTextureLayers_.empty())
                continue;

            const auto layer = freeTextureLayers_.back();
            freeTextureLayers_.pop_back();
            textureLeases_.emplace(key, TextureLease{ layer, 1u, false, false });
            layers[i] = layer;
            keys[i] = std::move(key);
            uploadNeeded[i] = i < payload.textures.size() && payload.textures[i].valid;
        }
        return layers;
    }

    void WorldStreamer::release_texture_layers(std::size_t assetSlot)
    {
        if (assetSlot >= assetTextureKeys_.size())
            return;
        for (const auto& key : assetTextureKeys_[assetSlot])
        {
            if (key.empty())
                continue;
            const auto found = textureLeases_.find(key);
            if (found == textureLeases_.end() || found->second.pinned)
                continue;
            if (found->second.references > 1)
            {
                found->second.references--;
                continue;
            }
            freeTextureLayers_.push_back(found->second.layer);
            textureLeases_.erase(found);
        }
        assetTextureKeys_[assetSlot].clear();
    }

    bool WorldStreamer::integrate_ready(std::chrono::steady_clock::time_point deadline)
    {
        if (!runtime_)
            return false;
        bool changed = false;

        // A large model can reference many DDS files. Upload only one layer
        // per frame and keep the decoded payload staged until all of its
        // layers are ready; this prevents one completed DG/SMOD from turning
        // into a long render-thread upload spike.
        if (staged_)
        {
            auto& staged = *staged_;
            while (staged.nextTexture < staged.layers.size())
            {
                const auto i = staged.nextTexture++;
                if (!staged.uploadNeeded[i] || staged.layers[i] == UINT32_MAX)
                    continue;
                const bool uploaded = renderer_->upload_streamed_asset_texture_layer(
                    staged.layers[i], staged.payload.textures[i]);
                if (uploaded)
                {
                    const auto key = phoenix::assets::lower_ascii(
                        staged.payload.texturePaths[i].string());
                    if (const auto lease = textureLeases_.find(key);
                        lease != textureLeases_.end())
                        lease->second.uploaded = true;
                }
                else
                {
                    // A bad texture must not make valid geometry disappear.
                    // UINT32_MAX remaps just that surface to its vertex-colour
                    // fallback; the asset itself remains resident and visible.
                    staged.layers[i] = UINT32_MAX;
                }
                staged.payload.textures[i] = {};
                break;
            }
            if (staged.nextTexture >= staged.layers.size())
            {
                const auto slot = staged.payload.slot;
                queuedSlots_.erase(slot);
                if (runtime_->install_world_asset_payload(
                    std::move(staged.payload), staged.layers))
                    changed = true;
                else
                {
                    release_texture_layers(slot);
                    failedSlots_.insert(slot);
                }
                staged_.reset();
            }
            return changed;
        }

        std::size_t integrated = 0;
        for (std::size_t i = 0; i < pending_.size()
            && integrated < kMaxIntegrationsPerFrame
            && std::chrono::steady_clock::now() < deadline;)
        {
            auto& pending = pending_[i];
            if (pending.future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            {
                ++i;
                continue;
            }

            const auto slot = pending.slot;
            PhoenixRuntime::WorldAssetPayload payload{};
            try { payload = pending.future.get(); }
            catch (...) { failedSlots_.insert(slot); }
            pending = std::move(pending_.back());
            pending_.pop_back();

            if (!payload.valid)
            {
                queuedSlots_.erase(slot);
                failedSlots_.insert(slot);
                continue;
            }
            StagedAsset staged{};
            staged.layers = acquire_texture_layers(payload, staged.uploadNeeded);
            staged.payload = std::move(payload);
            staged_ = std::move(staged);
            ++integrated;
            break;
        }
        return changed;
    }

    void WorldStreamer::launch_requests(const PhoenixRuntime::WorldStreamingDemand& demand)
    {
        if (!runtime_ || !scheduler_)
            return;
        for (const auto& request : demand.requests)
        {
            if (pending_.size() >= kMaxConcurrentLoads)
                break;
            if (queuedSlots_.contains(request.slot) || failedSlots_.contains(request.slot))
                continue;
            queuedSlots_.insert(request.slot);
            const auto slot = request.slot;
            const auto width = textureWidth_;
            const auto height = textureHeight_;
            const auto mips = textureMipLevels_;
            pending_.push_back({ slot, scheduler_->submit([runtime = runtime_, slot, width, height, mips]() {
                return runtime->load_world_asset_payload(slot, width, height, mips);
            }) });
        }
    }

    WorldStreamer::UpdateResult WorldStreamer::update(
        float cameraX, float cameraZ, float viewDistance,
        std::chrono::microseconds integrationBudget)
    {
        UpdateResult result{};
        if (!runtime_)
            return result;

        const auto deadline = std::chrono::steady_clock::now() + integrationBudget;
        result.sceneChanged = integrate_ready(deadline);

        const float loadDistance = std::clamp(viewDistance + 96.0f, 180.0f, 950.0f);
        const float unloadDistance = std::max(loadDistance + 160.0f, loadDistance * 1.35f);
        auto demand = runtime_->update_world_streaming_demand(
            cameraX, cameraZ, loadDistance, unloadDistance);
        result.sceneChanged = result.sceneChanged || demand.instancesChanged;

        for (const auto slot : demand.evictions)
        {
            if (queuedSlots_.contains(slot))
                continue;
            runtime_->evict_world_asset(slot);
            release_texture_layers(slot);
            result.sceneChanged = true;
        }
        launch_requests(demand);

        result.pendingAssets = pending_.size() + (staged_ ? 1u : 0u);
        for (const auto& asset : runtime_->state().worldAssets)
            if (asset.loaded)
                ++result.residentAssets;
        return result;
    }

    void WorldStreamer::reset()
    {
        for (auto& pending : pending_)
        {
            try { pending.future.get(); }
            catch (...) {}
        }
        pending_.clear();
        if (staged_)
        {
            release_texture_layers(staged_->payload.slot);
            staged_.reset();
        }
        queuedSlots_.clear();
        failedSlots_.clear();

        if (runtime_)
        {
            for (std::size_t slot = 0; slot < assetTextureKeys_.size(); ++slot)
            {
                release_texture_layers(slot);
                if (slot < runtime_->state().worldAssets.size()
                    && runtime_->state().worldAssets[slot].loaded)
                    runtime_->evict_world_asset(slot);
            }
        }
        assetTextureKeys_.clear();
        textureLeases_.clear();
        freeTextureLayers_.clear();
        runtime_ = nullptr;
        renderer_ = nullptr;
        scheduler_ = nullptr;
        firstTextureLayer_ = 0;
        textureLayerCapacity_ = 0;
        textureWidth_ = 0;
        textureHeight_ = 0;
        textureMipLevels_ = 0;
    }
}
