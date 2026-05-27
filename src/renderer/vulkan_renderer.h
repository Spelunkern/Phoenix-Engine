#pragma once

#include "volk.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace phoenix::renderer
{
    struct DdsTexture;

    struct TerrainVertex
    {
        float position[3]{};
        float color[3]{};
        float normal[3]{};
        float uv[2]{};
        std::uint32_t textureLayer{ 0xFFFFFFFFu };
    };

    struct ObjectInstance
    {
        float right[4]{};
        float up[4]{};
        float forward[4]{};
        float position[4]{};
    };

    struct ObjectBatch
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        std::uint32_t firstInstance{};
        std::uint32_t instanceCount{};
    };

    struct TerrainDrawRange
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
    };

    class VulkanRenderer
    {
    public:
        VulkanRenderer() = default;
        VulkanRenderer(const VulkanRenderer&) = delete;
        VulkanRenderer& operator=(const VulkanRenderer&) = delete;
        ~VulkanRenderer();

        bool initialize(SDL_Window* window, std::uint32_t width, std::uint32_t height);
        bool initialize_imgui(SDL_Window* window);
        void begin_imgui_frame();
        bool set_preview_image(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& bgraPixels);
        void enter_loading_mode();
        bool set_terrain_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool update_terrain_vertices(const std::vector<TerrainVertex>& vertices);
        bool set_static_object_mesh(
            const std::vector<TerrainVertex>& vertices,
            const std::vector<std::uint32_t>& indices,
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        bool update_static_object_instances(
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        bool set_animated_object_mesh(
            const std::vector<TerrainVertex>& vertices,
            const std::vector<std::uint32_t>& indices,
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        bool update_animated_object_scene(
            const std::vector<TerrainVertex>& vertices,
            const std::vector<ObjectInstance>& instances);
        bool update_terrain_indices(const std::vector<std::uint32_t>& indices);
        void set_static_object_batches(const std::vector<ObjectBatch>& batches);
        void set_animated_object_batches(const std::vector<ObjectBatch>& batches);
        void set_terrain_draw_ranges(const std::vector<TerrainDrawRange>& ranges);
        bool set_debug_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        void set_debug_visible(bool visible);
        bool set_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool update_character_vertices(const std::vector<TerrainVertex>& vertices);
        void set_character_visible(bool visible);
        bool upload_terrain_textures(const std::vector<DdsTexture>& textures);
        void set_sky_settings(const float* fogColor, float fogStartDistance, float fogEndDistance, bool hasWorldSky);
        void set_sky_texture_layers(std::uint32_t skyLayer, std::uint32_t primaryCloudLayer, std::uint32_t secondaryCloudLayer);
        void set_sky_tuning(const float* values, std::uint32_t count);
        void set_water_layer(std::uint32_t waterLayer);
        void set_water_animation(std::uint32_t baseLayer, std::uint32_t frameCount, float tileSize);
        void update_water_time(float totalTime);
        bool upload_terrain_texture_map(
            const std::vector<std::uint8_t>& data,
            std::uint32_t side,
            float mapSize,
            const float* tileSizes,
            std::uint32_t tileSizeCount);
        void set_camera(float x, float y, float z, float yaw, float pitch, float aspect, float farPlane);
        bool resize(std::uint32_t width, std::uint32_t height);
        void render_frame();
        void shutdown();

        bool ready() const { return ready_; }
        const std::string& adapter_name() const { return adapterName_; }
        std::uint32_t surface_width() const;
        std::uint32_t surface_height() const;
        std::uint64_t vram_total_bytes() const;
        std::uint64_t vram_used_bytes() const;

    private:
        bool create_instance(SDL_Window* window);
        bool create_surface(SDL_Window* window);
        bool select_device();
        bool create_device();
        bool create_swapchain(std::uint32_t width, std::uint32_t height);
        bool create_render_pass();
        bool create_framebuffers();
        bool create_commands();
        bool create_sync();
        bool create_depth_resources();
        bool create_terrain_pipeline();
        bool create_static_object_pipeline();
        bool create_sky_pipeline();
        bool create_descriptor_resources();
        bool create_preview_buffer(std::size_t byteSize);
        bool create_preview_image(std::uint32_t width, std::uint32_t height);
        bool create_host_buffer(
            const void* data,
            std::size_t byteSize,
            std::uint32_t usage,
            VkBuffer& bufferOut,
            VkDeviceMemory& memoryOut);
        bool create_device_local_buffer(
            const void* data,
            std::size_t byteSize,
            std::uint32_t usage,
            VkBuffer& bufferOut,
            VkDeviceMemory& memoryOut);
        bool load_shader_module(const char* relativePath, VkShaderModule& moduleOut);
        std::uint32_t find_memory_type(std::uint32_t typeBits, std::uint32_t requiredFlags) const;
        void destroy_swapchain();
        VkCommandBuffer begin_single_command() const;
        void end_single_command(VkCommandBuffer cmd) const;

        bool ready_{};
        bool imguiReady_{};
        bool imguiFrameStarted_{};
        std::string adapterName_;
        std::uint32_t graphicsQueueFamily_{ UINT32_MAX };
        std::uint32_t frameIndex_{};

        struct Impl;
        Impl* impl_{};
    };
}
