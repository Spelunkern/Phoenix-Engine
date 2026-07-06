#pragma once

// Internal renderer header: the private OpenGLRenderer::Impl definition and the
// small file-local helpers, shared across the renderer translation units
// (opengl_renderer.cpp + per-subsystem .cpp files). Not part of the public API —
// include only from renderer/*.cpp.
//
// Backend: OpenGL 4.5 core. The renderer was originally ported from a Vulkan
// backend; class/file names have since been renamed to match (OpenGLRenderer,
// opengl_renderer.h) instead of keeping the old Vulkan-era naming.

#include "renderer/opengl_renderer.h"
#include "renderer/dds_loader.h"
#include "renderer/gl/gl_loader.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace phoenix::renderer
{
    inline constexpr std::uint32_t kMaxFramesInFlight = 2;

    inline void log_line(const char* /*message*/) {}

    // Directory containing the running executable. Used so shaders resolve
    // whether the binary is launched from the repo root, the build folder,
    // or an install dir — and on Linux, where the CWD is rarely the repo.
    inline std::filesystem::path executable_dir()
    {
        std::error_code ec;
#if defined(__linux__)
        std::filesystem::path self = std::filesystem::canonical("/proc/self/exe", ec);
        if (!ec)
            return self.parent_path();
#endif
        return std::filesystem::current_path(ec);
    }

    // Format tags: see dds_loader.h's kDdsFormat* constants (kept numerically
    // identical to the old VkFormat enum values).
    inline constexpr std::uint32_t kFormatR8G8B8A8Unorm = kDdsFormatR8G8B8A8Unorm;
    inline constexpr std::uint32_t kFormatBc1RgbaUnormBlock = kDdsFormatBc1RgbaUnormBlock;
    inline constexpr std::uint32_t kFormatBc2UnormBlock = kDdsFormatBc2UnormBlock;
    inline constexpr std::uint32_t kFormatBc3UnormBlock = kDdsFormatBc3UnormBlock;

    inline bool is_bc_format(std::uint32_t format)
    {
        return format == kFormatBc1RgbaUnormBlock
            || format == kFormatBc2UnormBlock
            || format == kFormatBc3UnormBlock;
    }

    inline std::uint32_t bc_block_bytes(std::uint32_t format)
    {
        return format == kFormatBc1RgbaUnormBlock ? 8u : 16u;
    }

    inline GLenum bc_gl_internal_format(std::uint32_t format)
    {
        if (format == kFormatBc1RgbaUnormBlock) return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
        if (format == kFormatBc2UnormBlock) return GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
        return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    }

    inline std::vector<std::uint8_t> make_bc_fallback_mip(std::uint32_t format, std::uint32_t width, std::uint32_t height)
    {
        const auto blockBytes = bc_block_bytes(format);
        const auto blocksW = std::max(1u, (width + 3u) / 4u);
        const auto blocksH = std::max(1u, (height + 3u) / 4u);
        std::vector<std::uint8_t> data(static_cast<std::size_t>(blocksW) * blocksH * blockBytes);

        // Solid muted green in BC form; only used for missing/invalid layers.
        constexpr std::uint16_t c0 = 0x03E0; // green RGB565
        constexpr std::uint16_t c1 = 0x0000;
        for (std::size_t offset = 0; offset < data.size(); offset += blockBytes)
        {
            if (format == kFormatBc2UnormBlock)
            {
                std::memset(data.data() + offset, 0xFF, 8);
                data[offset + 8] = static_cast<std::uint8_t>(c0 & 0xFF);
                data[offset + 9] = static_cast<std::uint8_t>(c0 >> 8);
                data[offset + 10] = static_cast<std::uint8_t>(c1 & 0xFF);
                data[offset + 11] = static_cast<std::uint8_t>(c1 >> 8);
            }
            else if (format == kFormatBc3UnormBlock)
            {
                data[offset + 0] = 255;
                data[offset + 1] = 255;
                data[offset + 8] = static_cast<std::uint8_t>(c0 & 0xFF);
                data[offset + 9] = static_cast<std::uint8_t>(c0 >> 8);
                data[offset + 10] = static_cast<std::uint8_t>(c1 & 0xFF);
                data[offset + 11] = static_cast<std::uint8_t>(c1 >> 8);
            }
            else
            {
                data[offset + 0] = static_cast<std::uint8_t>(c0 & 0xFF);
                data[offset + 1] = static_cast<std::uint8_t>(c0 >> 8);
                data[offset + 2] = static_cast<std::uint8_t>(c1 & 0xFF);
                data[offset + 3] = static_cast<std::uint8_t>(c1 >> 8);
            }
        }
        return data;
    }

    struct GlBuffer
    {
        GLuint id{};
        std::size_t byteSize{};
        void* mapped{}; // persistent-mapped pointer, or null
    };

    struct GlTexture
    {
        GLuint id{};
        GLenum target{ GL_TEXTURE_2D_ARRAY };
    };

    struct OpenGLRenderer::Impl
    {
        // --- window / context ---
        SDL_Window* window{};
        void* glContext{}; // SDL_GLContext
        std::uint32_t surfaceWidth{};
        std::uint32_t surfaceHeight{};

        // --- programs ---
        GLuint terrainProgram{};
        GLuint staticObjectProgram{};
        GLuint skinnedCharacterProgram{};
        GLuint skyProgram{};
        bool skyReady{};

        // --- VAOs ---
        GLuint terrainVao{};
        GLuint waterVao{};
        GLuint objectVao{};
        GLuint animatedObjectVao{};
        GLuint characterVao{};
        GLuint botCharacterVao{};
        GLuint monsterCharacterVao{};
        GLuint npcCharacterVao{};
        GLuint emptyVao{}; // for sky (no vertex attribs, gl_VertexID driven)

        // --- camera uniform buffer (matches the old 60-float / 15-vec4 push
        // constant block; std140, binding = 0) ---
        GLuint cameraUbo{};

        // --- terrain ---
        GlBuffer terrainVertexBuffer;
        GlBuffer terrainIndexBuffer;
        std::uint32_t terrainIndexCount{};
        std::vector<TerrainDrawRange> terrainDrawRanges;
        std::size_t terrainVertexBytes{};
        bool terrainReady{};

        GlBuffer waterVertexBuffer;
        GlBuffer waterIndexBuffer;
        std::uint32_t waterIndexCount{};
        bool waterReady{};

        GlBuffer objectVertexBuffer;
        GlBuffer objectIndexBuffer;
        GlBuffer objectInstanceBuffer;
        std::vector<ObjectBatch> objectBatches;
        bool objectsReady{};

        GlBuffer animatedObjectVertexBuffer;
        GlBuffer animatedObjectIndexBuffer;
        GlBuffer animatedObjectInstanceBuffer;
        std::size_t animatedObjectVertexBytes{};
        std::size_t animatedObjectInstanceBytes{};
        std::vector<ObjectBatch> animatedObjectBatches;
        bool animatedObjectsReady{};

        GlBuffer characterVertexBuffer;
        GlBuffer characterIndexBuffer;
        std::uint32_t characterIndexCount{};
        std::size_t characterVertexBytes{};
        bool characterReady{};
        bool characterVisible{};

        GlBuffer botCharacterVertexBuffer;
        GlBuffer botCharacterIndexBuffer;
        GlBuffer botCharacterInstanceBuffer;
        std::vector<ObjectBatch> botCharacterBatches;
        bool botCharacterReady{};
        bool botCharacterVisible{};

        GlBuffer monsterCharacterVertexBuffer;
        GlBuffer monsterCharacterIndexBuffer;
        GlBuffer monsterCharacterInstanceBuffer[kMaxFramesInFlight];
        std::vector<ObjectBatch> monsterCharacterBatches;
        bool monsterCharacterReady{};
        bool monsterCharacterVisible{};
        bool monsterCharacterSkinned{};
        GlBuffer monsterPaletteBuffer[kMaxFramesInFlight];

        GlBuffer npcCharacterVertexBuffer;
        GlBuffer npcCharacterIndexBuffer;
        GlBuffer npcCharacterInstanceBuffer[kMaxFramesInFlight];
        std::vector<ObjectBatch> npcCharacterBatches;
        bool npcCharacterReady{};
        bool npcCharacterVisible{};
        GlBuffer npcPaletteBuffer[kMaxFramesInFlight];

        // --- textures ---
        GLuint terrainSampler{};
        GlTexture terrainTextureArray;
        std::uint32_t terrainTextureLayerCount{};
        std::uint32_t terrainTextureWidth{};
        std::uint32_t terrainTextureHeight{};
        std::uint32_t terrainTextureMipLevels{};
        std::uint32_t terrainTextureFormat{};
        bool terrainTextureCompressed{};
        bool terrainTexturesReady{};
        std::uint32_t maxImageArrayLayers{ 2048 };

        GlBuffer terrainMapBuffer;
        bool terrainMapReady{};

        GLuint lightmapSampler{};
        GlTexture lightmapTexture;
        std::uint32_t lightmapSectionCount{};
        bool lightmapReady{};

        // --- preview image (pre-scene UI, e.g. login screen) ---
        GLuint previewTexture{};
        std::uint32_t previewWidth{};
        std::uint32_t previewHeight{};
        bool previewReady{};

        // --- GPU frustum culling: kept as unused stub state, matching the
        // original Vulkan renderer where upload_indirect_draw_data() early-
        // returns false permanently ("single indirect buffer is not frame-
        // safe" — see the comment preserved in opengl_renderer.cpp). Indirect
        // draw / compute culling is therefore intentionally never exercised;
        // static objects always draw via the direct per-batch path.
        bool indirectReady{};
        std::uint32_t indirectBatchCount{};

        // --- GPU timing queries ---
        GLuint timestampQueryBegin[kMaxFramesInFlight]{};
        GLuint timestampQueryEnd[kMaxFramesInFlight]{};
        bool gpuQueriesReady{};
        float gpuFrameTimeMs{};
        std::uint64_t fragmentInvocations{};
        std::uint64_t vertexInvocations{};

        // --- ImGui ---
        struct ImguiIconTexture
        {
            GLuint texture{};
        };
        std::vector<ImguiIconTexture> imguiIconTextures;

        float cameraConstants[12]{
            // [0-2] position, [3] yaw, [4] pitch, [5] aspect, [6] tanHalfFov, [7] farPlane
            0.0f, 360.0f, -950.0f, 0.0f,
            -0.32f, 16.0f / 9.0f, 0.7002f, 5000.0f,
            // [8] cosYaw, [9] sinYaw, [10] cosPitch, [11] sinPitch (CPU-precomputed)
            1.0f, 0.0f, 1.0f, 0.0f,
        };
        float skyConstants[16]{
            0.42f, 0.58f, 0.74f, 0.0f,
            800.0f, 4200.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f, // waterInfo: baseLayer, frameCount, time, tileSize
        };
        float skyTuning[12]{
            0.62f, 0.0f, 1.0f, 0.0f,
            1.35f, 0.78f, 0.34f, 0.12f,
            0.82f, 1.10f, 0.22f, 0.06f,
        };
        float waterStyle[4]{ 0.10f, 0.18f, 0.22f, 0.62f }; // natural water tint
        float characterShading[16]{
            1.0f, 1.0f, 1.0f, 1.0f,
            0.56f, 0.56f, 0.56f, 0.72f,
            0.56f, 0.56f, 0.56f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
        };
        float assetShading[16]{
            1.0f, 1.0f, 1.0f, 1.0f,
            0.50f, 0.50f, 1.7f, 1.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 0.0f,
        };

        std::string adapterName;
    };
}
