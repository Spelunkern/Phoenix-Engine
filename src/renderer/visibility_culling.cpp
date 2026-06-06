#include "renderer/visibility_culling.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace phoenix::renderer
{
    namespace
    {
        constexpr float kTanHalfFov = 0.7002f;
    }

    bool sphere_visible(
        const CameraView& view,
        float worldX,
        float worldY,
        float worldZ,
        float radius)
    {
        const float dx = worldX - view.x;
        const float dy = worldY - view.y;
        const float dz = worldZ - view.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        const float maxDistance = view.distance + radius;
        if (distanceSq > maxDistance * maxDistance)
            return false;

        const float cy = std::cos(view.yaw);
        const float sy = std::sin(view.yaw);
        const float cp = std::cos(view.pitch);
        const float sp = std::sin(view.pitch);

        const float cameraX = cy * dx - sy * dz;
        const float yawZ = sy * dx + cy * dz;
        const float cameraY = cp * dy - sp * yawZ;
        const float cameraZ = sp * dy + cp * yawZ;
        if (cameraZ < -radius)
            return false;

        const float zForBounds = std::max(cameraZ, 1.0f);
        const float horizontal = zForBounds * kTanHalfFov * view.aspect + radius;
        const float vertical = zForBounds * kTanHalfFov + radius;
        return std::abs(cameraX) <= horizontal && std::abs(cameraY) <= vertical;
    }

    bool project_world_to_screen(
        const CameraView& view,
        float worldX,
        float worldY,
        float worldZ,
        float width,
        float height,
        ScreenPoint& out)
    {
        const float dx = worldX - view.x;
        const float dy = worldY - view.y;
        const float dz = worldZ - view.z;
        const float cy = std::cos(view.yaw);
        const float sy = std::sin(view.yaw);
        const float cp = std::cos(view.pitch);
        const float sp = std::sin(view.pitch);

        const float cameraX = cy * dx - sy * dz;
        const float yawZ = sy * dx + cy * dz;
        const float cameraY = cp * dy - sp * yawZ;
        const float cameraZ = sp * dy + cp * yawZ;
        if (cameraZ <= 1.0f)
            return false;

        const float ndcX = cameraX / (cameraZ * kTanHalfFov * view.aspect);
        const float ndcY = cameraY / (cameraZ * kTanHalfFov);
        if (std::abs(ndcX) > 1.2f || std::abs(ndcY) > 1.2f)
            return false;

        out.x = (ndcX * 0.5f + 0.5f) * width;
        out.y = (0.5f - ndcY * 0.5f) * height;
        return true;
    }

    void build_visible_terrain_ranges(
        std::vector<TerrainDrawRange>& ranges,
        const phoenix::runtime::PhoenixRuntime& runtime,
        const CameraView& view,
        const phoenix::runtime::PhoenixRuntime::TerrainLodInfo& lod)
    {
        ranges.clear();
        if (lod.chunks.empty() || runtime.state().world.isDungeon)
            return;

        constexpr std::uint32_t kChunkQ = phoenix::runtime::PhoenixRuntime::kTerrainChunkQuads;
        const float cullDist = view.distance;

        struct SortEntry { float distSq; TerrainDrawRange range; };
        static std::vector<SortEntry> sorted;
        sorted.clear();

        for (std::uint32_t cz = 0; cz < lod.chunkCountZ; ++cz)
        {
            for (std::uint32_t cx = 0; cx < lod.chunkCountX; ++cx)
            {
                const auto qMinX = cx * kChunkQ;
                const auto qMinZ = cz * kChunkQ;
                const auto qMaxX = std::min(lod.grid, qMinX + kChunkQ);
                const auto qMaxZ = std::min(lod.grid, qMinZ + kChunkQ);
                const auto centerX = -lod.halfMap + (static_cast<float>(qMinX + qMaxX) * 0.5f) * lod.cellSize;
                const auto centerZ = -lod.halfMap + (static_cast<float>(qMinZ + qMaxZ) * 0.5f) * lod.cellSize;
                const auto extentX = static_cast<float>(qMaxX - qMinX) * lod.cellSize * 0.5f;
                const auto extentZ = static_cast<float>(qMaxZ - qMinZ) * lod.cellSize * 0.5f;
                const auto radius = std::sqrt(extentX * extentX + extentZ * extentZ) + 50.0f;
                if (!sphere_visible(view, centerX, 30.0f, centerZ, radius))
                    continue;

                const float dx = centerX - view.x;
                const float dz = centerZ - view.z;
                const float distSq = dx * dx + dz * dz;
                if (std::sqrt(distSq) - radius > cullDist)
                    continue;

                const auto chunkIdx = static_cast<std::size_t>(cz) * lod.chunkCountX + cx;
                const auto& cl = lod.chunks[chunkIdx][0];
                if (cl.indexCount > 0)
                {
                    TerrainDrawRange range{};
                    range.firstIndex = cl.firstIndex;
                    range.indexCount = cl.indexCount;
                    sorted.push_back({ distSq, range });
                }
            }
        }

        std::sort(sorted.begin(), sorted.end(),
            [](const SortEntry& a, const SortEntry& b) { return a.distSq < b.distSq; });

        ranges.reserve(sorted.size());
        for (const auto& e : sorted)
            ranges.push_back(e.range);
    }

    void sort_scene_front_to_back(
        phoenix::runtime::StaticObjectScene& scene,
        float camX,
        float camY,
        float camZ)
    {
        if (scene.batches.size() != scene.batchBounds.size() || scene.batches.size() < 2)
            return;

        const auto n = scene.batches.size();
        static std::vector<std::uint32_t> order;
        static std::vector<float> dists;
        order.resize(n);
        dists.resize(n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            order[i] = i;
            const auto& b = scene.batchBounds[i];
            const float dx = b.x - camX;
            const float dy = b.y - camY;
            const float dz = b.z - camZ;
            dists[i] = dx * dx + dy * dy + dz * dz;
        }

        std::sort(order.begin(), order.end(),
            [](std::uint32_t a, std::uint32_t b) { return dists[a] < dists[b]; });

        static std::vector<ObjectBatch> tmpBatches;
        static std::vector<phoenix::runtime::StaticObjectScene::BatchBounds> tmpBounds;
        tmpBatches.resize(n);
        tmpBounds.resize(n);
        for (std::uint32_t i = 0; i < n; ++i)
        {
            tmpBatches[i] = scene.batches[order[i]];
            tmpBounds[i] = scene.batchBounds[order[i]];
        }
        scene.batches.swap(tmpBatches);
        scene.batchBounds.swap(tmpBounds);
    }

    std::vector<BatchBoundsGpu> extract_gpu_bounds(
        const phoenix::runtime::StaticObjectScene& scene)
    {
        std::vector<BatchBoundsGpu> gpuBounds(scene.batchBounds.size());
        for (std::size_t i = 0; i < scene.batchBounds.size(); ++i)
        {
            gpuBounds[i].x = scene.batchBounds[i].x;
            gpuBounds[i].y = scene.batchBounds[i].y;
            gpuBounds[i].z = scene.batchBounds[i].z;
            gpuBounds[i].radius = scene.batchBounds[i].radius;
        }
        return gpuBounds;
    }

    void build_visible_object_batches(
        const phoenix::runtime::StaticObjectScene& scene,
        const CameraView& view,
        std::vector<ObjectBatch>& batches)
    {
        batches.clear();
        if (scene.batches.empty() || scene.batchBounds.size() != scene.batches.size())
            return;

        struct SortEntry { float distSq; std::size_t index; };
        static std::vector<SortEntry> sorted;
        sorted.clear();

        for (std::size_t i = 0; i < scene.batches.size(); ++i)
        {
            const auto& bounds = scene.batchBounds[i];
            if (!sphere_visible(view, bounds.x, bounds.y, bounds.z, bounds.radius))
                continue;

            const float dx = bounds.x - view.x;
            const float dy = bounds.y - view.y;
            const float dz = bounds.z - view.z;
            sorted.push_back({ dx * dx + dy * dy + dz * dz, i });
        }

        std::sort(sorted.begin(), sorted.end(),
            [](const SortEntry& a, const SortEntry& b) { return a.distSq < b.distSq; });

        batches.reserve(sorted.size());
        for (const auto& e : sorted)
            batches.push_back(scene.batches[e.index]);
    }

    void build_visible_animated_batches(
        const phoenix::runtime::AnimatedObjectScene& scene,
        const CameraView& view,
        std::vector<ObjectBatch>& batches)
    {
        batches.clear();
        if (scene.batches.empty() || scene.batchBounds.size() != scene.batches.size())
            return;

        struct SortEntry { float distSq; std::size_t index; };
        static std::vector<SortEntry> sorted;
        sorted.clear();

        for (std::size_t i = 0; i < scene.batches.size(); ++i)
        {
            const auto& bounds = scene.batchBounds[i];
            if (!sphere_visible(view, bounds.x, bounds.y, bounds.z, bounds.radius))
                continue;

            const float dx = bounds.x - view.x;
            const float dy = bounds.y - view.y;
            const float dz = bounds.z - view.z;
            sorted.push_back({ dx * dx + dy * dy + dz * dz, i });
        }

        std::sort(sorted.begin(), sorted.end(),
            [](const SortEntry& a, const SortEntry& b) { return a.distSq < b.distSq; });

        batches.reserve(sorted.size());
        for (const auto& e : sorted)
            batches.push_back(scene.batches[e.index]);
    }
}
