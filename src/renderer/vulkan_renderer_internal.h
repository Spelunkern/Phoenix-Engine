#pragma once

// Internal renderer header: the private VulkanRenderer::Impl definition and the
// small file-local helpers, shared across the renderer translation units
// (vulkan_renderer.cpp + per-subsystem .cpp files). Not part of the public API —
// include only from renderer/*.cpp.

#include "renderer/vulkan_renderer.h"
#include "core/logging.h"

#include "volk.h"

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
    inline constexpr std::uint32_t kMaxFramesInFlight = 3;

    inline void log_line(const char* message)
    {
        phoenix::core::write_log_line("Renderer", message);
    }

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

    inline bool is_bc_format(VkFormat format)
    {
        return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK
            || format == VK_FORMAT_BC2_UNORM_BLOCK
            || format == VK_FORMAT_BC3_UNORM_BLOCK;
    }

    inline std::uint32_t bc_block_bytes(VkFormat format)
    {
        return format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? 8u : 16u;
    }

    inline std::vector<std::uint8_t> make_bc_fallback_mip(VkFormat format, std::uint32_t width, std::uint32_t height)
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
            if (format == VK_FORMAT_BC2_UNORM_BLOCK)
            {
                std::memset(data.data() + offset, 0xFF, 8);
                data[offset + 8] = static_cast<std::uint8_t>(c0 & 0xFF);
                data[offset + 9] = static_cast<std::uint8_t>(c0 >> 8);
                data[offset + 10] = static_cast<std::uint8_t>(c1 & 0xFF);
                data[offset + 11] = static_cast<std::uint8_t>(c1 >> 8);
            }
            else if (format == VK_FORMAT_BC3_UNORM_BLOCK)
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

    struct VulkanRenderer::Impl
    {
        VkInstance instance{};
        VkSurfaceKHR surface{};
        VkPhysicalDevice physicalDevice{};
        VkDevice device{};
        VkQueue graphicsQueue{};
        VkSwapchainKHR swapchain{};
        VkFormat swapchainFormat{};
        VkExtent2D swapchainExtent{};
        VkRenderPass renderPass{};
        VkImage depthImage{};
        VkDeviceMemory depthMemory{};
        VkImageView depthView{};
        VkPipelineLayout terrainPipelineLayout{};
        VkPipeline terrainPipeline{};
        VkPipeline staticObjectPipeline{};
        VkPipelineLayout skyPipelineLayout{};
        VkPipeline skyPipeline{};

        // Procedural weapon-effect particle resources (separate from terrain).
        VkDescriptorSetLayout particleSetLayout{};
        VkDescriptorPool particleDescriptorPool{};
        VkDescriptorSet particleDescriptorSet{};
        VkPipelineLayout particlePipelineLayout{};
        VkPipeline particlePipelineAlpha{};
        VkPipeline particlePipelineAdditive{};
        VkBuffer particleInstanceBuffer{};
        VkDeviceMemory particleInstanceMemory{};
        void* particleInstanceMapped{};           // persistent mapping
        std::size_t particleInstanceCapacity{};   // bytes
        std::uint32_t particleInstanceCount{};
        std::uint32_t particleAdditiveStart{};
        bool particlePipelineReady{};

        VkCommandPool commandPool{};
        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkFramebuffer> framebuffers;
        std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers{};
        std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable{};
        std::array<VkSemaphore, kMaxFramesInFlight> renderFinished{};
        std::array<VkFence, kMaxFramesInFlight> inFlight{};
        VkBuffer previewBuffer{};
        VkDeviceMemory previewMemory{};
        VkDeviceSize previewBufferSize{};
        VkImage previewImage{};
        VkDeviceMemory previewImageMemory{};
        std::uint32_t previewWidth{};
        std::uint32_t previewHeight{};
        bool previewReady{};
        bool previewUploadLogged{};
        VkBuffer terrainVertexBuffer{};
        VkDeviceMemory terrainVertexMemory{};
        VkBuffer terrainIndexBuffer{};
        VkDeviceMemory terrainIndexMemory{};
        std::uint32_t terrainIndexCount{};
        TerrainDrawRange waterDrawRange{};
        std::vector<TerrainDrawRange> terrainDrawRanges;
        std::size_t terrainVertexBytes{};
        bool terrainReady{};
        VkBuffer objectVertexBuffer{};
        VkDeviceMemory objectVertexMemory{};
        VkBuffer objectIndexBuffer{};
        VkDeviceMemory objectIndexMemory{};
        VkBuffer objectInstanceBuffer{};
        VkDeviceMemory objectInstanceMemory{};
        std::vector<ObjectBatch> objectBatches;
        bool objectsReady{};
        VkBuffer animatedObjectVertexBuffer{};
        VkDeviceMemory animatedObjectVertexMemory{};
        void* animatedObjectVertexMapped{};    // persistent mapping
        VkBuffer animatedObjectIndexBuffer{};
        VkDeviceMemory animatedObjectIndexMemory{};
        VkBuffer animatedObjectInstanceBuffer{};
        VkDeviceMemory animatedObjectInstanceMemory{};
        void* animatedObjectInstanceMapped{};  // persistent mapping
        std::size_t animatedObjectVertexBytes{};
        std::size_t animatedObjectInstanceBytes{};
        std::vector<ObjectBatch> animatedObjectBatches;
        bool animatedObjectsReady{};
        VkBuffer debugVertexBuffer{};
        VkDeviceMemory debugVertexMemory{};
        VkBuffer debugIndexBuffer{};
        VkDeviceMemory debugIndexMemory{};
        std::uint32_t debugIndexCount{};
        bool debugReady{};
        bool debugVisible{};
        VkBuffer characterVertexBuffer{};
        VkDeviceMemory characterVertexMemory{};
        VkBuffer characterIndexBuffer{};
        VkDeviceMemory characterIndexMemory{};
        std::uint32_t characterIndexCount{};
        std::size_t characterVertexBytes{};
        std::size_t characterVertexCapacity{};   // allocated capacity for vertex buffer
        void* characterVertexMapped{};
        std::size_t characterIndexCapacity{};    // allocated capacity for index buffer (bytes)
        bool characterReady{};
        bool characterVisible{};
        VkBuffer botCharacterVertexBuffer{};
        VkDeviceMemory botCharacterVertexMemory{};
        void* botCharacterVertexMapped{};
        VkBuffer botCharacterIndexBuffer{};
        VkDeviceMemory botCharacterIndexMemory{};
        VkBuffer botCharacterInstanceBuffer{};
        VkDeviceMemory botCharacterInstanceMemory{};
        void* botCharacterInstanceMapped{};
        std::size_t botCharacterVertexBytes{};
        std::size_t botCharacterVertexCapacity{};
        std::size_t botCharacterInstanceBytes{};
        std::size_t botCharacterInstanceCapacity{};
        std::vector<ObjectBatch> botCharacterBatches;
        bool botCharacterReady{};
        bool botCharacterVisible{};

        VkDescriptorPool descriptorPool{};
        VkDescriptorSetLayout descriptorSetLayout{};
        VkDescriptorSet descriptorSet{};
        VkSampler terrainSampler{};
        VkImage terrainTextureArray{};
        VkDeviceMemory terrainTextureArrayMemory{};
        VkImageView terrainTextureArrayView{};
        std::uint32_t terrainTextureLayerCount{};
        std::uint32_t terrainTextureWidth{};
        std::uint32_t terrainTextureHeight{};
        std::uint32_t terrainTextureMipLevels{};
        VkFormat terrainTextureFormat{ VK_FORMAT_UNDEFINED };
        bool terrainTextureCompressed{};
        bool terrainTexturesReady{};
        bool samplerAnisotropySupported{};
        float maxSamplerAnisotropy{ 1.0f };
        VkBuffer terrainMapBuffer{};
        VkDeviceMemory terrainMapMemory{};
        bool terrainMapReady{};

        // GPU frustum culling + indirect draw resources.
        VkPipelineLayout cullPipelineLayout{};
        VkPipeline cullPipeline{};
        VkDescriptorSetLayout cullDescriptorSetLayout{};
        VkDescriptorPool cullDescriptorPool{};
        VkDescriptorSet cullDescriptorSet{};
        VkBuffer indirectTemplateBuffer{};   // readonly: original draw commands
        VkDeviceMemory indirectTemplateMemory{};
        VkBuffer indirectBoundsBuffer{};     // readonly: bounding spheres
        VkDeviceMemory indirectBoundsMemory{};
        VkBuffer indirectDrawBuffer{};       // compute output: filtered commands
        VkDeviceMemory indirectDrawMemory{};
        std::uint32_t indirectBatchCount{};
        bool indirectReady{};
        bool multiDrawIndirectSupported{};
        // Integrated GPUs (Intel/AMD APU) use unified memory. Their fast memory
        // type is DEVICE_LOCAL | HOST_VISIBLE; the plain HOST_VISIBLE | HOST_COHERENT
        // type is often write-combined/uncached for the GPU, which makes per-frame
        // GPU-read/written buffers (skinned vertices, matrices, instances) crawl.
        // We use this flag to prefer the unified cached type on such hardware.
        bool integratedGpu{};

        // GPU compute skinning resources.
        VkPipelineLayout skinPipelineLayout{};
        VkPipeline skinPipeline{};
        VkDescriptorSetLayout skinDescriptorSetLayout{};
        VkDescriptorPool skinDescriptorPool{};
        VkDescriptorSet skinDescriptorSet{};
        VkBuffer skinSourceBuffer{};            // readonly: bind-pose source vertices
        VkDeviceMemory skinSourceMemory{};
        VkBuffer skinMatrixBuffer{};            // readonly: per-frame bone matrices
        VkDeviceMemory skinMatrixMemory{};
        void* skinMatrixMapped{};              // persistent mapping
        std::size_t skinMatrixBufferBytes{};
        std::uint32_t skinSourceVertexCount{};
        bool skinPipelineReady{};
        bool skinSourceReady{};

        // Per-frame skin dispatch queue.
        struct SkinDispatch
        {
            std::uint32_t firstVertex;
            std::uint32_t vertexCount;
            std::uint32_t boneMatrixOffset;
        };
        std::vector<SkinDispatch> skinDispatches;

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        VkPipelineCache pipelineCache{};

        float cameraConstants[8]{
            0.0f, 360.0f, -950.0f, 0.0f,
            -0.32f, 16.0f / 9.0f, 0.7002f, 5000.0f,
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
    };
}
