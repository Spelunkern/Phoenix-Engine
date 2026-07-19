#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SDL_Window;
struct ImFont;

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

    // Bind-pose vertex for GPU skinning (mobs/NPCs). Matches the skinned_character
    // shader's VSInput: TerrainVertex fields + per-vertex bone influences. bones
    // are GLOBAL mesh-bone indices (into the entity's palette); 3 used + pad.
    struct SkinnedVertex
    {
        float position[3]{};
        float color[3]{};
        float normal[3]{};
        float uv[2]{};
        std::uint32_t textureLayer{ 0xFFFFFFFFu };
        std::uint32_t bones[4]{};
        float weights[4]{};
    };

    struct ObjectBatch
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        std::uint32_t firstInstance{};
        std::uint32_t instanceCount{};
    };

    // Per-instance data for one effect particle: right/up/forward are the
    // world-space basis vectors already scaled+rotated (so the shader only
    // needs worldPos = position + right*localX + up*localY + forward*localZ —
    // for a billboard quad localZ is always 0, so forward is unused/zero).
    // color is straight (non-premultiplied) RGBA sampled from the effect's
    // keyframes.
    struct EffectParticleInstance
    {
        float right[4]{};
        float up[4]{};
        float forward[4]{};
        float position[4]{};
        float color[4]{};
    };

    // Like ObjectBatch, but effect particles are drawn with a blend mode taken
    // from the .EFT component (D3DBLEND-style enum values straight from the
    // file); the renderer maps them to GL blend factors per batch.
    struct EffectParticleBatch
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
        std::uint32_t firstInstance{};
        std::uint32_t instanceCount{};
        std::int32_t sourceBlend{};
        std::int32_t destinationBlend{};
    };

    struct BatchBoundsGpu
    {
        float x{};
        float y{};
        float z{};
        float radius{};
    };

    struct TerrainDrawRange
    {
        std::uint32_t firstIndex{};
        std::uint32_t indexCount{};
    };

    class OpenGLRenderer
    {
    public:
        OpenGLRenderer() = default;
        OpenGLRenderer(const OpenGLRenderer&) = delete;
        OpenGLRenderer& operator=(const OpenGLRenderer&) = delete;
        ~OpenGLRenderer();

        bool initialize(SDL_Window* window, std::uint32_t width, std::uint32_t height);
        bool initialize_imgui(SDL_Window* window);
        // Small Arial font for in-world NPC labels (nullptr if Arial wasn't found,
        // in which case the caller should use the default ImGui font).
        ImFont* npc_label_font() const { return npcLabelFont_; }
        void begin_imgui_frame();
        std::uint64_t upload_imgui_icon_rgba(
            const std::uint8_t* rgba,
            std::uint32_t width,
            std::uint32_t height);
        bool set_preview_image(std::uint32_t width, std::uint32_t height, const std::vector<std::uint8_t>& bgraPixels);
        void enter_loading_mode();
        bool set_terrain_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool set_water_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
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
        bool update_animated_object_vertices_range(
            const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount);
        bool update_terrain_indices(const std::vector<std::uint32_t>& indices);
        void set_static_object_batches(const std::vector<ObjectBatch>& batches);
        bool upload_indirect_draw_data(
            const std::vector<ObjectBatch>& batches,
            const std::vector<BatchBoundsGpu>& bounds);
        void update_indirect_draw_data(
            const std::vector<ObjectBatch>& batches,
            const std::vector<BatchBoundsGpu>& bounds);
        bool indirect_draw_ready() const;
        void set_animated_object_batches(const std::vector<ObjectBatch>& batches);
        // Rebuilds the effect-particle draw buffers from scratch; called once
        // per frame from the CPU particle simulation (cheap: particle counts
        // are small, unlike the terrain/static-object meshes). Vertices carry
        // one quad per distinct (texture, blend) batch; `batches` groups the
        // instance range that shares each quad.
        bool update_effect_particles(
            const std::vector<TerrainVertex>& vertices,
            const std::vector<std::uint32_t>& indices,
            const std::vector<EffectParticleInstance>& instances,
            const std::vector<EffectParticleBatch>& batches);
        void set_terrain_draw_ranges(const std::vector<TerrainDrawRange>& ranges);
        bool set_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        // Fast mesh swap: reuses existing GPU buffers when large enough, no vkDeviceWaitIdle.
        bool update_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool update_character_vertices(const std::vector<TerrainVertex>& vertices);
        bool update_character_vertices(const TerrainVertex* data, std::size_t count);
        void set_character_visible(bool visible);
        bool set_bot_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool update_bot_character_vertices(const std::vector<TerrainVertex>& vertices);
        bool update_bot_character_vertices_range(
            const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount);
        bool update_bot_character_instances(
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        void set_bot_character_visible(bool visible);
        bool set_monster_character_mesh(const std::vector<TerrainVertex>& vertices, const std::vector<std::uint32_t>& indices);
        bool update_monster_character_vertices(const std::vector<TerrainVertex>& vertices);
        bool update_monster_character_vertices_range(
            const TerrainVertex* vertices, std::uint32_t firstVertex, std::uint32_t vertexCount);
        bool update_monster_character_instances(
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        void set_monster_character_visible(bool visible);

        // GPU-skinned mob path. The bind mesh (SkinnedVertex) is uploaded once
        // per (re)build; each frame only the bone palette + instances change.
        // The palette is a flat float array, 16 floats (4 rows) per bone, all
        // entities' palettes concatenated; per-instance palette base bone index
        // is bit-packed into ObjectInstance.right[3].
        bool set_monster_skinned_mesh(const std::vector<SkinnedVertex>& vertices,
            const std::vector<std::uint32_t>& indices);
        void update_monster_bone_palette(const float* rows16PerBone, std::size_t floatCount);

        // GPU-skinned NPC path (dedicated buffers, same skinned pipeline).
        bool set_npc_skinned_mesh(const std::vector<SkinnedVertex>& vertices,
            const std::vector<std::uint32_t>& indices);
        void update_npc_bone_palette(const float* rows16PerBone, std::size_t floatCount);
        bool update_npc_skinned_instances(
            const std::vector<ObjectInstance>& instances,
            const std::vector<ObjectBatch>& batches);
        void set_npc_skinned_visible(bool visible);
        // `pump` (optional) is invoked periodically during the CPU staging
        // phase so the caller can keep processing window messages — the
        // upload takes seconds and the app must stay closable throughout.
        bool upload_terrain_textures(const std::vector<DdsTexture>& textures,
            const std::function<void()>& pump = {});
        bool upload_terrain_texture_layers(std::uint32_t firstLayer, const std::vector<DdsTexture>& textures);
        // Dedicated array for the "Effects" debug panel (see effect_particle
        // .frag's high-bit texture-layer convention). Recreates the array
        // wholesale from the full accumulated texture list each call — the
        // caller (main.cpp) appends new files' textures to the end and never
        // reorders, so earlier debug spawns' layer indices stay valid.
        bool upload_debug_effect_textures(const std::vector<DdsTexture>& textures);
        bool upload_field_lightmaps(const std::vector<DdsTexture>& lightmaps, std::uint32_t sectionCount);
        void disable_field_lightmaps();
        void set_sky_settings(const float* fogColor, float fogStartDistance, float fogEndDistance, bool hasWorldSky);
        void set_sky_texture_layers(std::uint32_t skyLayer, std::uint32_t primaryCloudLayer, std::uint32_t secondaryCloudLayer);
        void set_sky_tuning(const float* values, std::uint32_t count);
        // Live toggle for MSAA + alpha-to-coverage — safe to call every
        // frame regardless of whether the context actually has multisample
        // buffers (glEnable/glDisable(GL_MULTISAMPLE) is a no-op without
        // them, never an error).
        void set_antialiasing_enabled(bool enabled);
        void set_water_layer(std::uint32_t waterLayer);
        void set_water_animation(std::uint32_t baseLayer, std::uint32_t frameCount, float tileSize);
        void update_water_time(float totalTime);
        bool upload_terrain_texture_map(
            const std::vector<std::uint8_t>& data,
            std::uint32_t side,
            float mapSize,
            const float* tileSizes,
            std::uint32_t tileSizeCount,
            std::uint32_t alphaMaskLayerFlags = 0,
            std::uint32_t splatLayerCount = 0);
        void set_camera(float x, float y, float z, float yaw, float pitch, float aspect, float farPlane);
        bool resize(std::uint32_t width, std::uint32_t height);
        // Recreate the swapchain at the surface's current extent regardless of the
        // cached size (used when the swapchain goes out-of-date, e.g. minimize/restore).
        bool recreate_swapchain();
        void wait_for_frame();
        void render_frame();
        void shutdown();

        bool ready() const { return ready_; }
        const std::string& adapter_name() const { return adapterName_; }
        std::uint32_t surface_width() const;
        std::uint32_t surface_height() const;
        std::uint64_t vram_total_bytes() const;
        std::uint64_t vram_used_bytes() const;

        struct GpuMetrics
        {
            float frameTimeMs{};
            std::uint64_t fragmentInvocations{};
            std::uint64_t vertexInvocations{};
            bool available{};
        };
        GpuMetrics gpu_metrics() const;

    private:
        bool create_terrain_pipeline();
        bool create_static_object_pipeline();
        bool create_effect_particle_pipeline();
        bool create_skinned_character_pipeline();
        bool create_cull_compute_pipeline();
        bool create_sky_pipeline();
        bool create_descriptor_resources();
        bool create_preview_buffer(std::size_t byteSize);
        bool create_preview_image(std::uint32_t width, std::uint32_t height);
        void destroy_swapchain();

        bool ready_{};
        bool imguiReady_{};
        bool imguiFrameStarted_{};
        ImFont* npcLabelFont_{};
        std::string adapterName_;
        std::uint32_t graphicsQueueFamily_{ UINT32_MAX };
        std::uint32_t frameIndex_{};

        struct Impl;
        Impl* impl_{};
    };
}
