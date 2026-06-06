#pragma once

#include "renderer/vulkan_renderer.h"
#include "runtime/phoenix_runtime.h"

#include <vector>

namespace phoenix::renderer
{
    struct CameraView
    {
        float x{};
        float y{};
        float z{};
        float yaw{};
        float pitch{};
        float aspect{ 1.0f };
        float distance{ 5000.0f };
    };

    struct ScreenPoint
    {
        float x{};
        float y{};
    };

    bool sphere_visible(
        const CameraView& view,
        float worldX,
        float worldY,
        float worldZ,
        float radius);

    bool project_world_to_screen(
        const CameraView& view,
        float worldX,
        float worldY,
        float worldZ,
        float width,
        float height,
        ScreenPoint& out);

    void build_visible_terrain_ranges(
        std::vector<TerrainDrawRange>& ranges,
        const phoenix::runtime::PhoenixRuntime& runtime,
        const CameraView& view,
        const phoenix::runtime::PhoenixRuntime::TerrainLodInfo& lod);

    void sort_scene_front_to_back(
        phoenix::runtime::StaticObjectScene& scene,
        float camX,
        float camY,
        float camZ);

    std::vector<BatchBoundsGpu> extract_gpu_bounds(
        const phoenix::runtime::StaticObjectScene& scene);

    void build_visible_object_batches(
        const phoenix::runtime::StaticObjectScene& scene,
        const CameraView& view,
        std::vector<ObjectBatch>& batches);

    void build_visible_animated_batches(
        const phoenix::runtime::AnimatedObjectScene& scene,
        const CameraView& view,
        std::vector<ObjectBatch>& batches);
}
