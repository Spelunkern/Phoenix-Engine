#include "renderer/vulkan_renderer.h"
#include "renderer/dds_loader.h"
#include "platform/sdl_window.h"

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_sdl2.h"
#include "volk.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <future>
#include <limits>
#include <thread>
#include <vector>

namespace phoenix::renderer
{
    namespace
    {
        constexpr std::uint32_t kMaxFramesInFlight = 3;

        void log_line(const char* message)
        {
            std::ofstream log("PhoenixEngine.log", std::ios::app);
            log << message << "\n";
        }
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
        VkBuffer animatedObjectIndexBuffer{};
        VkDeviceMemory animatedObjectIndexMemory{};
        VkBuffer animatedObjectInstanceBuffer{};
        VkDeviceMemory animatedObjectInstanceMemory{};
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
        bool characterReady{};
        bool characterVisible{};

        VkDescriptorPool descriptorPool{};
        VkDescriptorSetLayout descriptorSetLayout{};
        VkDescriptorSet descriptorSet{};
        VkSampler terrainSampler{};
        VkImage terrainTextureArray{};
        VkDeviceMemory terrainTextureArrayMemory{};
        VkImageView terrainTextureArrayView{};
        std::uint32_t terrainTextureLayerCount{};
        bool terrainTexturesReady{};
        bool samplerAnisotropySupported{};
        float maxSamplerAnisotropy{ 1.0f };
        VkBuffer terrainMapBuffer{};
        VkDeviceMemory terrainMapMemory{};
        bool terrainMapReady{};

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

    VulkanRenderer::~VulkanRenderer()
    {
        shutdown();
    }

    bool VulkanRenderer::initialize(SDL_Window* window, std::uint32_t width, std::uint32_t height)
    {
        impl_ = new Impl{};
        if (volkInitialize() != VK_SUCCESS)
        {
            log_line("Vulkan: volkInitialize failed");
            return false;
        }

        log_line("Vulkan: creating instance");
        if (!create_instance(window)
            || (log_line("Vulkan: creating surface"), !create_surface(window))
            || (log_line("Vulkan: selecting device"), !select_device())
            || (log_line("Vulkan: creating device"), !create_device())
            || (log_line("Vulkan: creating swapchain"), !create_swapchain(width, height))
            || (log_line("Vulkan: creating depth resources"), !create_depth_resources())
            || (log_line("Vulkan: creating render pass"), !create_render_pass())
            || (log_line("Vulkan: creating framebuffers"), !create_framebuffers())
            || (log_line("Vulkan: creating commands"), !create_commands())
            || (log_line("Vulkan: creating descriptors"), !create_descriptor_resources())
            || (log_line("Vulkan: creating terrain pipeline"), !create_terrain_pipeline())
            || (log_line("Vulkan: creating static object pipeline"), !create_static_object_pipeline())
            || (log_line("Vulkan: creating sky pipeline"), !create_sky_pipeline())
            || (log_line("Vulkan: creating sync"), !create_sync()))
        {
            shutdown();
            return false;
        }

        {
            VkPipelineCacheCreateInfo cacheInfo{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
            vkCreatePipelineCache(impl_->device, &cacheInfo, nullptr, &impl_->pipelineCache);
        }

        ready_ = true;
        log_line("Vulkan: initialized");
        return true;
    }

    bool VulkanRenderer::initialize_imgui(SDL_Window* window)
    {
        if (!ready_ || imguiReady_)
            return ready_;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();

        if (!ImGui_ImplSDL2_InitForVulkan(window))
            return false;

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_2;
        initInfo.Instance = impl_->instance;
        initInfo.PhysicalDevice = impl_->physicalDevice;
        initInfo.Device = impl_->device;
        initInfo.QueueFamily = graphicsQueueFamily_;
        initInfo.Queue = impl_->graphicsQueue;
        initInfo.DescriptorPoolSize = 128;
        initInfo.MinImageCount = 2;
        initInfo.ImageCount = static_cast<std::uint32_t>(impl_->swapchainImages.size());
        initInfo.PipelineInfoMain.RenderPass = impl_->renderPass;
        initInfo.PipelineInfoMain.Subpass = 0;
        initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            return false;
        }

        imguiReady_ = true;
        log_line("ImGui: initialized");
        return true;
    }

    void VulkanRenderer::begin_imgui_frame()
    {
        if (!imguiReady_)
            return;

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        imguiFrameStarted_ = true;
    }

    bool VulkanRenderer::create_instance(SDL_Window* window)
    {
        unsigned extCount = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr) || extCount == 0)
        {
            log_line("Vulkan: SDL_Vulkan_GetInstanceExtensions count failed");
            return false;
        }
        std::vector<const char*> extensions(extCount);
        if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, extensions.data()))
        {
            log_line("Vulkan: SDL_Vulkan_GetInstanceExtensions names failed");
            return false;
        }

        VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        appInfo.pApplicationName = "Phoenix Engine";
        appInfo.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.pEngineName = "Phoenix Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(0, 1, 0);
        appInfo.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        if (vkCreateInstance(&createInfo, nullptr, &impl_->instance) != VK_SUCCESS)
        {
            log_line("Vulkan: vkCreateInstance failed");
            return false;
        }

        volkLoadInstance(impl_->instance);
        return true;
    }

    bool VulkanRenderer::create_surface(SDL_Window* window)
    {
        if (!SDL_Vulkan_CreateSurface(window, impl_->instance, &impl_->surface))
        {
            log_line("Vulkan: SDL_Vulkan_CreateSurface failed");
            return false;
        }
        return true;
    }

    bool VulkanRenderer::select_device()
    {
        std::uint32_t deviceCount{};
        vkEnumeratePhysicalDevices(impl_->instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            log_line("Vulkan: no physical devices");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(impl_->instance, &deviceCount, devices.data());
        for (const auto device : devices)
        {
            std::uint32_t familyCount{};
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());

            for (std::uint32_t index = 0; index < familyCount; ++index)
            {
                VkBool32 presentSupported{};
                vkGetPhysicalDeviceSurfaceSupportKHR(device, index, impl_->surface, &presentSupported);
                if ((families[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentSupported)
                {
                    impl_->physicalDevice = device;
                    graphicsQueueFamily_ = index;

                    VkPhysicalDeviceProperties properties{};
                    vkGetPhysicalDeviceProperties(device, &properties);
                    adapterName_ = properties.deviceName;
                    impl_->maxSamplerAnisotropy = std::max(1.0f, properties.limits.maxSamplerAnisotropy);
                    return true;
                }
            }
        }

        log_line("Vulkan: no graphics+present queue");
        return false;
    }

    bool VulkanRenderer::create_device()
    {
        constexpr float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queueInfo.queueFamilyIndex = graphicsQueueFamily_;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkPhysicalDeviceFeatures supportedFeatures{};
        vkGetPhysicalDeviceFeatures(impl_->physicalDevice, &supportedFeatures);

        VkPhysicalDeviceFeatures features{};
        features.shaderSampledImageArrayDynamicIndexing = supportedFeatures.shaderSampledImageArrayDynamicIndexing;
        features.samplerAnisotropy = supportedFeatures.samplerAnisotropy;
        impl_->samplerAnisotropySupported = supportedFeatures.samplerAnisotropy == VK_TRUE;

        const char* extensions[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        createInfo.queueCreateInfoCount = 1;
        createInfo.pQueueCreateInfos = &queueInfo;
        createInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(extensions));
        createInfo.ppEnabledExtensionNames = extensions;
        createInfo.pEnabledFeatures = &features;

        if (vkCreateDevice(impl_->physicalDevice, &createInfo, nullptr, &impl_->device) != VK_SUCCESS)
        {
            log_line("Vulkan: vkCreateDevice failed");
            return false;
        }

        volkLoadDevice(impl_->device);
        vkGetDeviceQueue(impl_->device, graphicsQueueFamily_, 0, &impl_->graphicsQueue);
        vkGetPhysicalDeviceMemoryProperties(impl_->physicalDevice, &impl_->memoryProperties);
        return true;
    }

    bool VulkanRenderer::create_swapchain(std::uint32_t width, std::uint32_t height)
    {
        VkSurfaceCapabilitiesKHR capabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(impl_->physicalDevice, impl_->surface, &capabilities);

        std::uint32_t formatCount{};
        vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physicalDevice, impl_->surface, &formatCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(impl_->physicalDevice, impl_->surface, &formatCount, formats.data());

        auto chosenFormat = formats[0];
        for (const auto& format : formats)
        {
            if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosenFormat = format;
        }

        impl_->swapchainFormat = chosenFormat.format;
        impl_->swapchainExtent = capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()
            ? capabilities.currentExtent
            : VkExtent2D{
                std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
                std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height),
            };

        const auto desiredImageCount = std::min(
            std::max(capabilities.minImageCount + 1, 3u),
            capabilities.maxImageCount == 0 ? 3u : capabilities.maxImageCount);

        VkSwapchainCreateInfoKHR createInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        createInfo.surface = impl_->surface;
        createInfo.minImageCount = desiredImageCount;
        createInfo.imageFormat = impl_->swapchainFormat;
        createInfo.imageColorSpace = chosenFormat.colorSpace;
        createInfo.imageExtent = impl_->swapchainExtent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        createInfo.preTransform = capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

        auto chosenPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        {
            std::uint32_t modeCount{};
            vkGetPhysicalDeviceSurfacePresentModesKHR(impl_->physicalDevice, impl_->surface, &modeCount, nullptr);
            std::vector<VkPresentModeKHR> modes(modeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(impl_->physicalDevice, impl_->surface, &modeCount, modes.data());
            for (const auto mode : modes)
            {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                {
                    chosenPresentMode = VK_PRESENT_MODE_MAILBOX_KHR;
                    break;
                }
            }
        }
        createInfo.presentMode = chosenPresentMode;
        createInfo.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(impl_->device, &createInfo, nullptr, &impl_->swapchain) != VK_SUCCESS)
        {
            log_line("Vulkan: vkCreateSwapchainKHR failed");
            return false;
        }

        std::uint32_t imageCount{};
        vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &imageCount, nullptr);
        impl_->swapchainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(impl_->device, impl_->swapchain, &imageCount, impl_->swapchainImages.data());

        impl_->swapchainImageViews.resize(imageCount);
        for (std::size_t i = 0; i < impl_->swapchainImages.size(); ++i)
        {
            VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            viewInfo.image = impl_->swapchainImages[i];
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = impl_->swapchainFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            if (vkCreateImageView(impl_->device, &viewInfo, nullptr, &impl_->swapchainImageViews[i]) != VK_SUCCESS)
            {
                log_line("Vulkan: vkCreateImageView failed");
                return false;
            }
        }
        return true;
    }

    bool VulkanRenderer::create_render_pass()
    {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = impl_->swapchainFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = VK_FORMAT_D32_SFLOAT;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorReference{};
        colorReference.attachment = 0;
        colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthReference{};
        depthReference.attachment = 1;
        depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorReference;
        subpass.pDepthStencilAttachment = &depthReference;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        const VkAttachmentDescription attachments[] = { colorAttachment, depthAttachment };
        VkRenderPassCreateInfo createInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        createInfo.attachmentCount = static_cast<std::uint32_t>(std::size(attachments));
        createInfo.pAttachments = attachments;
        createInfo.subpassCount = 1;
        createInfo.pSubpasses = &subpass;
        createInfo.dependencyCount = 1;
        createInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(impl_->device, &createInfo, nullptr, &impl_->renderPass) != VK_SUCCESS)
        {
            log_line("Vulkan: vkCreateRenderPass failed");
            return false;
        }
        return true;
    }

    bool VulkanRenderer::create_framebuffers()
    {
        impl_->framebuffers.resize(impl_->swapchainImageViews.size());
        for (std::size_t i = 0; i < impl_->swapchainImageViews.size(); ++i)
        {
            VkImageView attachments[] = { impl_->swapchainImageViews[i], impl_->depthView };
            VkFramebufferCreateInfo createInfo{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            createInfo.renderPass = impl_->renderPass;
            createInfo.attachmentCount = static_cast<std::uint32_t>(std::size(attachments));
            createInfo.pAttachments = attachments;
            createInfo.width = impl_->swapchainExtent.width;
            createInfo.height = impl_->swapchainExtent.height;
            createInfo.layers = 1;
            if (vkCreateFramebuffer(impl_->device, &createInfo, nullptr, &impl_->framebuffers[i]) != VK_SUCCESS)
            {
                log_line("Vulkan: vkCreateFramebuffer failed");
                return false;
            }
        }
        return true;
    }

    bool VulkanRenderer::create_commands()
    {
        VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphicsQueueFamily_;
        if (vkCreateCommandPool(impl_->device, &poolInfo, nullptr, &impl_->commandPool) != VK_SUCCESS)
        {
            log_line("Vulkan: vkCreateCommandPool failed");
            return false;
        }

        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = impl_->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = kMaxFramesInFlight;
        if (vkAllocateCommandBuffers(impl_->device, &allocInfo, impl_->commandBuffers.data()) != VK_SUCCESS)
        {
            log_line("Vulkan: vkAllocateCommandBuffers failed");
            return false;
        }
        return true;
    }

    bool VulkanRenderer::create_sync()
    {
        VkSemaphoreCreateInfo semaphoreInfo{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (vkCreateSemaphore(impl_->device, &semaphoreInfo, nullptr, &impl_->imageAvailable[i]) != VK_SUCCESS
                || vkCreateSemaphore(impl_->device, &semaphoreInfo, nullptr, &impl_->renderFinished[i]) != VK_SUCCESS
                || vkCreateFence(impl_->device, &fenceInfo, nullptr, &impl_->inFlight[i]) != VK_SUCCESS)
            {
                log_line("Vulkan: sync creation failed");
                return false;
            }
        }
        return true;
    }

    bool VulkanRenderer::create_depth_resources()
    {
        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_D32_SFLOAT;
        imageInfo.extent = { impl_->swapchainExtent.width, impl_->swapchainExtent.height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(impl_->device, &imageInfo, nullptr, &impl_->depthImage) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(impl_->device, impl_->depthImage, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
            return false;

        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(impl_->device, &allocInfo, nullptr, &impl_->depthMemory) != VK_SUCCESS)
            return false;
        vkBindImageMemory(impl_->device, impl_->depthImage, impl_->depthMemory, 0);

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = impl_->depthImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        return vkCreateImageView(impl_->device, &viewInfo, nullptr, &impl_->depthView) == VK_SUCCESS;
    }

    std::uint32_t VulkanRenderer::surface_width() const
    {
        return impl_ ? impl_->swapchainExtent.width : 0;
    }

    std::uint32_t VulkanRenderer::surface_height() const
    {
        return impl_ ? impl_->swapchainExtent.height : 0;
    }

    std::uint64_t VulkanRenderer::vram_total_bytes() const
    {
        if (!impl_)
            return 0;
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < impl_->memoryProperties.memoryHeapCount; ++i)
        {
            if (impl_->memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                total += impl_->memoryProperties.memoryHeaps[i].size;
        }
        return total;
    }

    std::uint64_t VulkanRenderer::vram_used_bytes() const
    {
        if (!impl_ || !impl_->physicalDevice)
            return 0;
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budgetProps{};
        budgetProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        VkPhysicalDeviceMemoryProperties2 memProps2{};
        memProps2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        memProps2.pNext = &budgetProps;
        vkGetPhysicalDeviceMemoryProperties2(impl_->physicalDevice, &memProps2);
        std::uint64_t used = 0;
        for (std::uint32_t i = 0; i < memProps2.memoryProperties.memoryHeapCount; ++i)
        {
            if (memProps2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                used += budgetProps.heapUsage[i];
        }
        return used;
    }

    bool VulkanRenderer::resize(std::uint32_t width, std::uint32_t height)
    {
        if (!ready_ || !impl_ || !impl_->device)
            return false;
        if (width == 0 || height == 0)
            return false;
        if (impl_->swapchainExtent.width == width && impl_->swapchainExtent.height == height)
            return true;

        vkDeviceWaitIdle(impl_->device);
        destroy_swapchain();
        if (!create_swapchain(width, height)
            || !create_depth_resources()
            || !create_framebuffers())
        {
            ready_ = false;
            log_line("Vulkan: resize failed");
            return false;
        }

        log_line("Vulkan: swapchain resized");
        return true;
    }

    std::uint32_t VulkanRenderer::find_memory_type(std::uint32_t typeBits, std::uint32_t requiredFlags) const
    {
        const auto& properties = impl_->memoryProperties;
        for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i))
                && (properties.memoryTypes[i].propertyFlags & requiredFlags) == requiredFlags)
                return i;
        }
        return UINT32_MAX;
    }

    bool VulkanRenderer::load_shader_module(const char* relativePath, VkShaderModule& moduleOut)
    {
        const std::filesystem::path candidates[] = {
            std::filesystem::current_path() / relativePath,
            std::filesystem::current_path().parent_path().parent_path().parent_path() / relativePath,
        };

        std::vector<char> bytes;
        for (const auto& candidate : candidates)
        {
            std::ifstream input(candidate, std::ios::binary | std::ios::ate);
            if (!input)
                continue;
            const auto size = input.tellg();
            input.seekg(0, std::ios::beg);
            bytes.resize(static_cast<std::size_t>(size));
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            break;
        }
        if (bytes.empty())
            return false;

        VkShaderModuleCreateInfo createInfo{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        createInfo.codeSize = bytes.size();
        createInfo.pCode = reinterpret_cast<const std::uint32_t*>(bytes.data());
        return vkCreateShaderModule(impl_->device, &createInfo, nullptr, &moduleOut) == VK_SUCCESS;
    }

    VkCommandBuffer VulkanRenderer::begin_single_command() const
    {
        VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        allocInfo.commandPool = impl_->commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer cmd{};
        vkAllocateCommandBuffers(impl_->device, &allocInfo, &cmd);

        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &beginInfo);
        return cmd;
    }

    void VulkanRenderer::end_single_command(VkCommandBuffer cmd) const
    {
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        vkQueueSubmit(impl_->graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(impl_->graphicsQueue);

        vkFreeCommandBuffers(impl_->device, impl_->commandPool, 1, &cmd);
    }

    bool VulkanRenderer::create_descriptor_resources()
    {
        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = 1;

        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(impl_->device, &poolInfo, nullptr, &impl_->descriptorPool) != VK_SUCCESS)
        {
            log_line("Vulkan: descriptor pool creation failed");
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 2;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(impl_->device, &layoutInfo, nullptr, &impl_->descriptorSetLayout) != VK_SUCCESS)
        {
            log_line("Vulkan: descriptor set layout creation failed");
            return false;
        }

        VkSamplerCreateInfo samplerInfo{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.anisotropyEnable = impl_->samplerAnisotropySupported ? VK_TRUE : VK_FALSE;
        samplerInfo.maxAnisotropy = impl_->samplerAnisotropySupported
            ? std::min(16.0f, impl_->maxSamplerAnisotropy)
            : 1.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(impl_->device, &samplerInfo, nullptr, &impl_->terrainSampler) != VK_SUCCESS)
        {
            log_line("Vulkan: sampler creation failed");
            return false;
        }

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = impl_->descriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &impl_->descriptorSetLayout;
        if (vkAllocateDescriptorSets(impl_->device, &allocInfo, &impl_->descriptorSet) != VK_SUCCESS)
        {
            log_line("Vulkan: descriptor set allocation failed");
            return false;
        }

        {
            const std::uint32_t dummy[2]{ 0xFFFFFFFFu, 0 };
            if (!create_host_buffer(dummy, sizeof(dummy),
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                impl_->terrainMapBuffer, impl_->terrainMapMemory))
            {
                log_line("Vulkan: dummy terrain map buffer creation failed");
                return false;
            }

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = impl_->terrainMapBuffer;
            bufInfo.offset = 0;
            bufInfo.range = sizeof(dummy);

            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = impl_->descriptorSet;
            write.dstBinding = 1;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(impl_->device, 1, &write, 0, nullptr);
        }

        return true;
    }

    bool VulkanRenderer::create_terrain_pipeline()
    {
        VkShaderModule vertexShader{};
        VkShaderModule fragmentShader{};
        if (!load_shader_module("shaders\\compiled\\terrain.vert.spv", vertexShader)
            || !load_shader_module("shaders\\compiled\\terrain.frag.spv", fragmentShader))
        {
            log_line("Vulkan: could not load terrain shaders");
            return false;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(float) * 36;

        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &impl_->descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(impl_->device, &layoutInfo, nullptr, &impl_->terrainPipelineLayout) != VK_SUCCESS)
        {
            vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
            vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexShader;
        stages[0].pName = "VSMain";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentShader;
        stages[1].pName = "PSMain";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(TerrainVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attributes[5]{};
        attributes[0].location = 0;
        attributes[0].binding = 0;
        attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[0].offset = offsetof(TerrainVertex, position);
        attributes[1].location = 1;
        attributes[1].binding = 0;
        attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[1].offset = offsetof(TerrainVertex, color);
        attributes[2].location = 2;
        attributes[2].binding = 0;
        attributes[2].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributes[2].offset = offsetof(TerrainVertex, normal);
        attributes[3].location = 3;
        attributes[3].binding = 0;
        attributes[3].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[3].offset = offsetof(TerrainVertex, uv);
        attributes[4].location = 4;
        attributes[4].binding = 0;
        attributes[4].format = VK_FORMAT_R32_UINT;
        attributes[4].offset = offsetof(TerrainVertex, textureLayer);

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(attributes));
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(impl_->swapchainExtent.width);
        viewport.height = static_cast<float>(impl_->swapchainExtent.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = impl_->swapchainExtent;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo createInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        createInfo.stageCount = 2;
        createInfo.pStages = stages;
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pViewportState = &viewportState;
        createInfo.pRasterizationState = &raster;
        createInfo.pMultisampleState = &multisample;
        createInfo.pDepthStencilState = &depth;
        createInfo.pColorBlendState = &blend;
        createInfo.pDynamicState = &dynamicState;
        createInfo.layout = impl_->terrainPipelineLayout;
        createInfo.renderPass = impl_->renderPass;
        createInfo.subpass = 0;
        const auto ok = vkCreateGraphicsPipelines(impl_->device, impl_->pipelineCache, 1, &createInfo, nullptr, &impl_->terrainPipeline) == VK_SUCCESS;
        vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
        vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
        return ok;
    }

    bool VulkanRenderer::create_static_object_pipeline()
    {
        VkShaderModule vertexShader{};
        VkShaderModule fragmentShader{};
        if (!load_shader_module("shaders\\compiled\\static_object.vert.spv", vertexShader))
        {
            log_line("Vulkan: could not load static object vertex shader");
            return false;
        }
        if (!load_shader_module("shaders\\compiled\\static_object.frag.spv", fragmentShader))
        {
            vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
            log_line("Vulkan: could not load static object fragment shader");
            return false;
        }

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexShader;
        stages[0].pName = "VSMain";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentShader;
        stages[1].pName = "PSMain";

        VkVertexInputBindingDescription bindings[2]{};
        bindings[0].binding = 0;
        bindings[0].stride = sizeof(TerrainVertex);
        bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindings[1].binding = 1;
        bindings[1].stride = sizeof(ObjectInstance);
        bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        VkVertexInputAttributeDescription attributes[9]{};
        attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, position) };
        attributes[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, color) };
        attributes[2] = { 2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(TerrainVertex, normal) };
        attributes[3] = { 3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(TerrainVertex, uv) };
        attributes[4] = { 4, 0, VK_FORMAT_R32_UINT, offsetof(TerrainVertex, textureLayer) };
        attributes[5] = { 5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ObjectInstance, right) };
        attributes[6] = { 6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ObjectInstance, up) };
        attributes[7] = { 7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ObjectInstance, forward) };
        attributes[8] = { 8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(ObjectInstance, position) };

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(std::size(bindings));
        vertexInput.pVertexBindingDescriptions = bindings;
        vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(std::size(attributes));
        vertexInput.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(impl_->swapchainExtent.width);
        viewport.height = static_cast<float>(impl_->swapchainExtent.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = impl_->swapchainExtent;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo createInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        createInfo.stageCount = 2;
        createInfo.pStages = stages;
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pViewportState = &viewportState;
        createInfo.pRasterizationState = &raster;
        createInfo.pMultisampleState = &multisample;
        createInfo.pDepthStencilState = &depth;
        createInfo.pColorBlendState = &blend;
        createInfo.pDynamicState = &dynamicState;
        createInfo.layout = impl_->terrainPipelineLayout;
        createInfo.renderPass = impl_->renderPass;
        createInfo.subpass = 0;
        const auto ok = vkCreateGraphicsPipelines(impl_->device, impl_->pipelineCache, 1, &createInfo, nullptr, &impl_->staticObjectPipeline) == VK_SUCCESS;
        vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
        vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
        return ok;
    }

    bool VulkanRenderer::create_sky_pipeline()
    {
        VkShaderModule vertexShader{};
        VkShaderModule fragmentShader{};
        if (!load_shader_module("shaders\\compiled\\sky.vert.spv", vertexShader)
            || !load_shader_module("shaders\\compiled\\sky.frag.spv", fragmentShader))
        {
            log_line("Vulkan: sky shaders not found, sky will be clear color only");
            return true;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(float) * 36;

        VkPipelineLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &impl_->descriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(impl_->device, &layoutInfo, nullptr, &impl_->skyPipelineLayout) != VK_SUCCESS)
            return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexShader;
        stages[0].pName = "VSMain";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentShader;
        stages[1].pName = "PSMain";

        VkPipelineVertexInputStateCreateInfo vertexInput{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkViewport viewport{};
        viewport.width = static_cast<float>(impl_->swapchainExtent.width);
        viewport.height = static_cast<float>(impl_->swapchainExtent.height);
        viewport.maxDepth = 1.0f;
        VkRect2D scissor{};
        scissor.extent = impl_->swapchainExtent;
        VkPipelineViewportStateCreateInfo viewportState{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;
        VkDynamicState dynamicStates[]{ VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dynamicState.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
        dynamicState.pDynamicStates = dynamicStates;

        VkPipelineRasterizationStateCreateInfo raster{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        blend.attachmentCount = 1;
        blend.pAttachments = &blendAttachment;

        VkGraphicsPipelineCreateInfo createInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        createInfo.stageCount = 2;
        createInfo.pStages = stages;
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pViewportState = &viewportState;
        createInfo.pRasterizationState = &raster;
        createInfo.pMultisampleState = &multisample;
        createInfo.pDepthStencilState = &depth;
        createInfo.pColorBlendState = &blend;
        createInfo.pDynamicState = &dynamicState;
        createInfo.layout = impl_->skyPipelineLayout;
        createInfo.renderPass = impl_->renderPass;
        createInfo.subpass = 0;
        const auto ok = vkCreateGraphicsPipelines(impl_->device, impl_->pipelineCache, 1, &createInfo, nullptr, &impl_->skyPipeline) == VK_SUCCESS;
        vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
        vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
        return ok;
    }

    bool VulkanRenderer::upload_terrain_textures(const std::vector<DdsTexture>& textures)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);
        if (impl_->terrainTextureArrayView)
            vkDestroyImageView(impl_->device, impl_->terrainTextureArrayView, nullptr);
        if (impl_->terrainTextureArray)
            vkDestroyImage(impl_->device, impl_->terrainTextureArray, nullptr);
        if (impl_->terrainTextureArrayMemory)
            vkFreeMemory(impl_->device, impl_->terrainTextureArrayMemory, nullptr);
        impl_->terrainTextureArrayView = {};
        impl_->terrainTextureArray = {};
        impl_->terrainTextureArrayMemory = {};
        impl_->terrainTextureLayerCount = 0;
        impl_->terrainTexturesReady = false;

        if (textures.empty())
            return false;

        std::uint32_t texWidth = 0;
        std::uint32_t texHeight = 0;
        for (const auto& tex : textures)
        {
            if (tex.valid)
            {
                texWidth = tex.width;
                texHeight = tex.height;
                break;
            }
        }
        if (texWidth == 0 || texHeight == 0)
            return false;

        const auto layerCount = static_cast<std::uint32_t>(textures.size());
        const auto layerSize = static_cast<std::size_t>(texWidth) * texHeight * 4;
        const auto maxDim = std::max(texWidth, texHeight);
        // Cap mip chain: stop at 4x4 to avoid cross-layer bleeding at tiny mip levels.
        const auto fullMips = static_cast<std::uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
        const auto mipLevels = std::min(fullMips, static_cast<std::uint32_t>(std::max(1.0, std::log2(static_cast<double>(maxDim)) - 1.0)));
        std::vector<std::uint32_t> mipWidths(mipLevels);
        std::vector<std::uint32_t> mipHeights(mipLevels);
        for (std::uint32_t mip = 0; mip < mipLevels; ++mip)
        {
            mipWidths[mip] = std::max(1u, texWidth >> mip);
            mipHeights[mip] = std::max(1u, texHeight >> mip);
        }

        std::vector<std::uint8_t> basePixels(layerSize * layerCount);
        std::vector<std::uint8_t> fallbackPixels;
        for (std::uint32_t i = 0; i < layerCount; ++i)
        {
            auto* dst = basePixels.data() + static_cast<std::size_t>(i) * layerSize;
            if (i < textures.size() && textures[i].valid
                && textures[i].width == texWidth && textures[i].height == texHeight)
            {
                std::memcpy(dst, textures[i].rgba.data(),
                    std::min(layerSize, textures[i].rgba.size()));
            }
            else if (i < textures.size() && textures[i].valid)
            {
                for (std::uint32_t y = 0; y < texHeight; ++y)
                {
                    const auto sourceY = texHeight > 1
                        ? (static_cast<float>(y) + 0.5f) * static_cast<float>(textures[i].height) / static_cast<float>(texHeight) - 0.5f
                        : 0.0f;
                    const auto y0 = static_cast<std::uint32_t>(std::clamp(std::floor(sourceY), 0.0f, static_cast<float>(textures[i].height - 1)));
                    const auto y1 = std::min(textures[i].height - 1, y0 + 1);
                    const auto ty = std::clamp(sourceY - static_cast<float>(y0), 0.0f, 1.0f);
                    for (std::uint32_t x = 0; x < texWidth; ++x)
                    {
                        const auto sourceX = texWidth > 1
                            ? (static_cast<float>(x) + 0.5f) * static_cast<float>(textures[i].width) / static_cast<float>(texWidth) - 0.5f
                            : 0.0f;
                        const auto x0 = static_cast<std::uint32_t>(std::clamp(std::floor(sourceX), 0.0f, static_cast<float>(textures[i].width - 1)));
                        const auto x1 = std::min(textures[i].width - 1, x0 + 1);
                        const auto tx = std::clamp(sourceX - static_cast<float>(x0), 0.0f, 1.0f);
                        const auto out = (static_cast<std::size_t>(y) * texWidth + x) * 4;
                        for (std::size_t channel = 0; channel < 4; ++channel)
                        {
                            const auto c00 = textures[i].rgba[(static_cast<std::size_t>(y0) * textures[i].width + x0) * 4 + channel];
                            const auto c10 = textures[i].rgba[(static_cast<std::size_t>(y0) * textures[i].width + x1) * 4 + channel];
                            const auto c01 = textures[i].rgba[(static_cast<std::size_t>(y1) * textures[i].width + x0) * 4 + channel];
                            const auto c11 = textures[i].rgba[(static_cast<std::size_t>(y1) * textures[i].width + x1) * 4 + channel];
                            const auto top = std::lerp(static_cast<float>(c00), static_cast<float>(c10), tx);
                            const auto bottom = std::lerp(static_cast<float>(c01), static_cast<float>(c11), tx);
                            dst[out + channel] = static_cast<std::uint8_t>(std::clamp(std::lerp(top, bottom, ty), 0.0f, 255.0f));
                        }
                    }
                }
            }
            else
            {
                if (fallbackPixels.empty())
                {
                    fallbackPixels.resize(static_cast<std::size_t>(layerSize));
                    for (std::size_t p = 0; p < static_cast<std::size_t>(texWidth) * texHeight; ++p)
                    {
                        fallbackPixels[p * 4 + 0] = 90;
                        fallbackPixels[p * 4 + 1] = 130;
                        fallbackPixels[p * 4 + 2] = 60;
                        fallbackPixels[p * 4 + 3] = 255;
                    }
                }
                std::memcpy(dst, fallbackPixels.data(), layerSize);
            }
        }

        const auto stagingSize = static_cast<VkDeviceSize>(layerSize) * layerCount;

        VkBuffer stagingBuffer{};
        VkDeviceMemory stagingMemory{};
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = stagingSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(impl_->device, &bufferInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
            return false;

        VkMemoryRequirements bufReqs{};
        vkGetBufferMemoryRequirements(impl_->device, stagingBuffer, &bufReqs);
        auto memType = find_memory_type(bufReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memType == UINT32_MAX)
        {
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            return false;
        }

        VkMemoryAllocateInfo bufAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        bufAlloc.allocationSize = bufReqs.size;
        bufAlloc.memoryTypeIndex = memType;
        if (vkAllocateMemory(impl_->device, &bufAlloc, nullptr, &stagingMemory) != VK_SUCCESS)
        {
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            return false;
        }
        vkBindBufferMemory(impl_->device, stagingBuffer, stagingMemory, 0);

        void* mapped{};
        vkMapMemory(impl_->device, stagingMemory, 0, stagingSize, 0, &mapped);
        std::memcpy(mapped, basePixels.data(), basePixels.size());
        vkUnmapMemory(impl_->device, stagingMemory);

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageInfo.extent = { texWidth, texHeight, 1 };
        imageInfo.mipLevels = mipLevels;
        imageInfo.arrayLayers = layerCount;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateImage(impl_->device, &imageInfo, nullptr, &impl_->terrainTextureArray) != VK_SUCCESS)
        {
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }

        VkMemoryRequirements imgReqs{};
        vkGetImageMemoryRequirements(impl_->device, impl_->terrainTextureArray, &imgReqs);
        memType = find_memory_type(imgReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memType == UINT32_MAX)
        {
            vkDestroyImage(impl_->device, impl_->terrainTextureArray, nullptr);
            impl_->terrainTextureArray = {};
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }

        VkMemoryAllocateInfo imgAlloc{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        imgAlloc.allocationSize = imgReqs.size;
        imgAlloc.memoryTypeIndex = memType;
        if (vkAllocateMemory(impl_->device, &imgAlloc, nullptr, &impl_->terrainTextureArrayMemory) != VK_SUCCESS)
        {
            vkDestroyImage(impl_->device, impl_->terrainTextureArray, nullptr);
            impl_->terrainTextureArray = {};
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }
        vkBindImageMemory(impl_->device, impl_->terrainTextureArray, impl_->terrainTextureArrayMemory, 0);

        auto cmd = begin_single_command();

        {
            VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            toTransfer.srcAccessMask = 0;
            toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toTransfer.image = impl_->terrainTextureArray;
            toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            toTransfer.subresourceRange.levelCount = mipLevels;
            toTransfer.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &toTransfer);
        }

        for (std::uint32_t i = 0; i < layerCount; ++i)
        {
            VkBufferImageCopy region{};
            region.bufferOffset = static_cast<VkDeviceSize>(i) * layerSize;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.layerCount = 1;
            region.imageSubresource.baseArrayLayer = i;
            region.imageExtent = { texWidth, texHeight, 1 };
            vkCmdCopyBufferToImage(cmd, stagingBuffer, impl_->terrainTextureArray,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }

        for (std::uint32_t mip = 1; mip < mipLevels; ++mip)
        {
            // Transition previous mip level to TRANSFER_SRC for all layers.
            VkImageMemoryBarrier mipBarrier{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            mipBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            mipBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            mipBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            mipBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            mipBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mipBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            mipBarrier.image = impl_->terrainTextureArray;
            mipBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            mipBarrier.subresourceRange.baseMipLevel = mip - 1;
            mipBarrier.subresourceRange.levelCount = 1;
            mipBarrier.subresourceRange.layerCount = layerCount;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &mipBarrier);

            // Blit each layer individually to prevent cross-layer bleeding.
            for (std::uint32_t layer = 0; layer < layerCount; ++layer)
            {
            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = mip - 1;
            blit.srcSubresource.baseArrayLayer = layer;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {
                static_cast<std::int32_t>(mipWidths[mip - 1]),
                static_cast<std::int32_t>(mipHeights[mip - 1]),
                1 };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = mip;
            blit.dstSubresource.baseArrayLayer = layer;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {
                static_cast<std::int32_t>(mipWidths[mip]),
                static_cast<std::int32_t>(mipHeights[mip]),
                1 };
            vkCmdBlitImage(cmd,
                impl_->terrainTextureArray, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                impl_->terrainTextureArray, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, VK_FILTER_LINEAR);
            } // end per-layer blit
        }

        {
            std::array<VkImageMemoryBarrier, 2> finalBarriers{};
            std::uint32_t barrierCount = 0;

            if (mipLevels > 1)
            {
                auto& srcToShader = finalBarriers[barrierCount++];
                srcToShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                srcToShader.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                srcToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                srcToShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                srcToShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                srcToShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                srcToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                srcToShader.image = impl_->terrainTextureArray;
                srcToShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                srcToShader.subresourceRange.baseMipLevel = 0;
                srcToShader.subresourceRange.levelCount = mipLevels - 1;
                srcToShader.subresourceRange.layerCount = layerCount;
            }

            auto& dstToShader = finalBarriers[barrierCount++];
            dstToShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            dstToShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            dstToShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            dstToShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            dstToShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            dstToShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dstToShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            dstToShader.image = impl_->terrainTextureArray;
            dstToShader.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            dstToShader.subresourceRange.baseMipLevel = mipLevels - 1;
            dstToShader.subresourceRange.levelCount = 1;
            dstToShader.subresourceRange.layerCount = layerCount;

            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, barrierCount, finalBarriers.data());
        }

        end_single_command(cmd);

        vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
        vkFreeMemory(impl_->device, stagingMemory, nullptr);

        VkImageViewCreateInfo viewInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        viewInfo.image = impl_->terrainTextureArray;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = mipLevels;
        viewInfo.subresourceRange.layerCount = layerCount;
        if (vkCreateImageView(impl_->device, &viewInfo, nullptr, &impl_->terrainTextureArrayView) != VK_SUCCESS)
            return false;

        VkDescriptorImageInfo imageDescInfo{};
        imageDescInfo.sampler = impl_->terrainSampler;
        imageDescInfo.imageView = impl_->terrainTextureArrayView;
        imageDescInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = impl_->descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageDescInfo;
        vkUpdateDescriptorSets(impl_->device, 1, &write, 0, nullptr);

        impl_->terrainTextureLayerCount = layerCount;
        impl_->terrainTexturesReady = true;
        log_line("Vulkan: terrain textures uploaded");
        return true;
    }

    bool VulkanRenderer::create_preview_buffer(std::size_t byteSize)
    {
        if (impl_->previewBuffer && impl_->previewBufferSize >= byteSize)
            return true;

        if (impl_->previewBuffer)
            vkDestroyBuffer(impl_->device, impl_->previewBuffer, nullptr);
        if (impl_->previewMemory)
            vkFreeMemory(impl_->device, impl_->previewMemory, nullptr);
        impl_->previewBuffer = {};
        impl_->previewMemory = {};
        impl_->previewBufferSize = {};

        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = byteSize;
        bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(impl_->device, &bufferInfo, nullptr, &impl_->previewBuffer) != VK_SUCCESS)
        {
            log_line("Vulkan: preview buffer creation failed");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(impl_->device, impl_->previewBuffer, &requirements);
        const auto memoryType = find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX)
        {
            log_line("Vulkan: no host visible memory for preview");
            return false;
        }

        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(impl_->device, &allocInfo, nullptr, &impl_->previewMemory) != VK_SUCCESS)
        {
            log_line("Vulkan: preview memory allocation failed");
            return false;
        }

        vkBindBufferMemory(impl_->device, impl_->previewBuffer, impl_->previewMemory, 0);
        impl_->previewBufferSize = byteSize;
        return true;
    }

    bool VulkanRenderer::create_preview_image(std::uint32_t width, std::uint32_t height)
    {
        if (impl_->previewImage && impl_->previewWidth == width && impl_->previewHeight == height)
            return true;

        if (impl_->previewImage)
            vkDestroyImage(impl_->device, impl_->previewImage, nullptr);
        if (impl_->previewImageMemory)
            vkFreeMemory(impl_->device, impl_->previewImageMemory, nullptr);
        impl_->previewImage = {};
        impl_->previewImageMemory = {};

        VkImageCreateInfo imageInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = impl_->swapchainFormat;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(impl_->device, &imageInfo, nullptr, &impl_->previewImage) != VK_SUCCESS)
        {
            log_line("Vulkan: preview image creation failed");
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(impl_->device, impl_->previewImage, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
        {
            log_line("Vulkan: no device local memory for preview image");
            return false;
        }

        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(impl_->device, &allocInfo, nullptr, &impl_->previewImageMemory) != VK_SUCCESS)
        {
            log_line("Vulkan: preview image memory allocation failed");
            return false;
        }

        vkBindImageMemory(impl_->device, impl_->previewImage, impl_->previewImageMemory, 0);
        return true;
    }

    bool VulkanRenderer::set_preview_image(
        std::uint32_t width,
        std::uint32_t height,
        const std::vector<std::uint8_t>& bgraPixels)
    {
        if (!ready_ || width == 0 || height == 0 || bgraPixels.size() < static_cast<std::size_t>(width) * height * 4)
            return false;

        const auto byteSize = static_cast<std::size_t>(width) * height * 4;
        if (!create_preview_buffer(byteSize) || !create_preview_image(width, height))
            return false;

        void* mapped{};
        if (vkMapMemory(impl_->device, impl_->previewMemory, 0, byteSize, 0, &mapped) != VK_SUCCESS)
            return false;
        std::memcpy(mapped, bgraPixels.data(), byteSize);
        vkUnmapMemory(impl_->device, impl_->previewMemory);

        impl_->previewWidth = width;
        impl_->previewHeight = height;
        impl_->previewReady = true;
        if (!impl_->previewUploadLogged)
        {
            log_line("Vulkan: preview image uploaded to staging buffer");
            impl_->previewUploadLogged = true;
        }
        return true;
    }

    void VulkanRenderer::enter_loading_mode()
    {
        if (!impl_)
            return;
        vkDeviceWaitIdle(impl_->device);
        impl_->terrainReady = false;
        impl_->objectsReady = false;
        impl_->animatedObjectsReady = false;
        impl_->debugReady = false;
        impl_->characterReady = false;
    }

    bool VulkanRenderer::create_host_buffer(
        const void* data,
        std::size_t byteSize,
        std::uint32_t usage,
        VkBuffer& bufferOut,
        VkDeviceMemory& memoryOut)
    {
        if (byteSize == 0)
            return false;

        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = byteSize;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(impl_->device, &bufferInfo, nullptr, &bufferOut) != VK_SUCCESS)
            return false;

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(impl_->device, bufferOut, &requirements);
        const auto memoryType = find_memory_type(
            requirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (memoryType == UINT32_MAX)
            return false;

        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(impl_->device, &allocInfo, nullptr, &memoryOut) != VK_SUCCESS)
            return false;

        void* mapped{};
        if (vkMapMemory(impl_->device, memoryOut, 0, byteSize, 0, &mapped) != VK_SUCCESS)
            return false;
        std::memcpy(mapped, data, byteSize);
        vkUnmapMemory(impl_->device, memoryOut);
        vkBindBufferMemory(impl_->device, bufferOut, memoryOut, 0);
        return true;
    }

    bool VulkanRenderer::create_device_local_buffer(
        const void* data,
        std::size_t byteSize,
        std::uint32_t usage,
        VkBuffer& bufferOut,
        VkDeviceMemory& memoryOut)
    {
        if (byteSize == 0)
            return false;

        VkBuffer stagingBuffer{};
        VkDeviceMemory stagingMemory{};
        if (!create_host_buffer(data, byteSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingBuffer, stagingMemory))
            return false;

        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = byteSize;
        bufferInfo.usage = usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(impl_->device, &bufferInfo, nullptr, &bufferOut) != VK_SUCCESS)
        {
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }

        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(impl_->device, bufferOut, &requirements);
        const auto memoryType = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (memoryType == UINT32_MAX)
        {
            vkDestroyBuffer(impl_->device, bufferOut, nullptr);
            bufferOut = {};
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }

        VkMemoryAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        allocInfo.allocationSize = requirements.size;
        allocInfo.memoryTypeIndex = memoryType;
        if (vkAllocateMemory(impl_->device, &allocInfo, nullptr, &memoryOut) != VK_SUCCESS)
        {
            vkDestroyBuffer(impl_->device, bufferOut, nullptr);
            bufferOut = {};
            vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
            vkFreeMemory(impl_->device, stagingMemory, nullptr);
            return false;
        }
        vkBindBufferMemory(impl_->device, bufferOut, memoryOut, 0);

        auto cmd = begin_single_command();
        VkBufferCopy copyRegion{};
        copyRegion.size = byteSize;
        vkCmdCopyBuffer(cmd, stagingBuffer, bufferOut, 1, &copyRegion);
        end_single_command(cmd);

        vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
        vkFreeMemory(impl_->device, stagingMemory, nullptr);
        return true;
    }

    bool VulkanRenderer::set_terrain_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);

        if (impl_->terrainVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainVertexBuffer, nullptr);
        if (impl_->terrainVertexMemory)
            vkFreeMemory(impl_->device, impl_->terrainVertexMemory, nullptr);
        if (impl_->terrainIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainIndexBuffer, nullptr);
        if (impl_->terrainIndexMemory)
            vkFreeMemory(impl_->device, impl_->terrainIndexMemory, nullptr);
        impl_->terrainVertexBuffer = {};
        impl_->terrainVertexMemory = {};
        impl_->terrainIndexBuffer = {};
        impl_->terrainIndexMemory = {};
        impl_->terrainReady = false;
        impl_->terrainIndexCount = 0;
        impl_->waterDrawRange = {};
        impl_->terrainDrawRanges.clear();

        if (vertices.empty() || indices.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        if (!create_device_local_buffer(vertices.data(), vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, impl_->terrainVertexBuffer, impl_->terrainVertexMemory)
            || !create_device_local_buffer(indices.data(), indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, impl_->terrainIndexBuffer, impl_->terrainIndexMemory))
        {
            log_line("Vulkan: terrain mesh upload failed");
            return false;
        }

        impl_->terrainIndexCount = static_cast<std::uint32_t>(indices.size());
        if (impl_->terrainIndexCount >= 6)
        {
            impl_->waterDrawRange.firstIndex = impl_->terrainIndexCount - 6u;
            impl_->waterDrawRange.indexCount = 6u;
            impl_->terrainIndexCount -= 6u;
        }
        impl_->terrainVertexBytes = vertexBytes;
        impl_->terrainReady = true;
        log_line("Vulkan: terrain mesh uploaded");
        return true;
    }

    bool VulkanRenderer::update_terrain_vertices(const std::vector<TerrainVertex>& vertices)
    {
        if (!ready_ || !impl_->terrainReady || vertices.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        if (vertexBytes != impl_->terrainVertexBytes)
            return set_terrain_mesh(vertices, {}); // Size mismatch: fall back to full rebuild (shouldn't happen).

        // Create a staging buffer with the new vertex data.
        VkBuffer stagingBuffer{};
        VkDeviceMemory stagingMemory{};
        if (!create_host_buffer(vertices.data(), vertexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, stagingBuffer, stagingMemory))
            return false;

        // Copy staging → device-local vertex buffer using a one-shot command.
        auto cmd = begin_single_command();
        VkBufferCopy copyRegion{};
        copyRegion.size = vertexBytes;
        vkCmdCopyBuffer(cmd, stagingBuffer, impl_->terrainVertexBuffer, 1, &copyRegion);
        end_single_command(cmd); // Waits for queue idle internally.

        vkDestroyBuffer(impl_->device, stagingBuffer, nullptr);
        vkFreeMemory(impl_->device, stagingMemory, nullptr);
        return true;
    }

    bool VulkanRenderer::update_terrain_indices(const std::vector<std::uint32_t>& indices)
    {
        if (!ready_ || indices.empty())
            return false;

        vkDeviceWaitIdle(impl_->device);
        if (impl_->terrainIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainIndexBuffer, nullptr);
        if (impl_->terrainIndexMemory)
            vkFreeMemory(impl_->device, impl_->terrainIndexMemory, nullptr);
        impl_->terrainIndexBuffer = {};
        impl_->terrainIndexMemory = {};
        impl_->terrainReady = false;
        impl_->terrainIndexCount = 0;

        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        if (!create_device_local_buffer(indices.data(), indexBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            impl_->terrainIndexBuffer,
            impl_->terrainIndexMemory))
        {
            log_line("Vulkan: terrain index upload failed");
            return false;
        }

        impl_->terrainIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->terrainReady = impl_->terrainVertexBuffer != VK_NULL_HANDLE;
        return impl_->terrainReady;
    }

    bool VulkanRenderer::set_static_object_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);

        if (impl_->objectVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectVertexBuffer, nullptr);
        if (impl_->objectVertexMemory)
            vkFreeMemory(impl_->device, impl_->objectVertexMemory, nullptr);
        if (impl_->objectIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectIndexBuffer, nullptr);
        if (impl_->objectIndexMemory)
            vkFreeMemory(impl_->device, impl_->objectIndexMemory, nullptr);
        if (impl_->objectInstanceBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectInstanceBuffer, nullptr);
        if (impl_->objectInstanceMemory)
            vkFreeMemory(impl_->device, impl_->objectInstanceMemory, nullptr);

        impl_->objectVertexBuffer = {};
        impl_->objectVertexMemory = {};
        impl_->objectIndexBuffer = {};
        impl_->objectIndexMemory = {};
        impl_->objectInstanceBuffer = {};
        impl_->objectInstanceMemory = {};
        impl_->objectBatches.clear();
        impl_->objectsReady = false;

        if (vertices.empty() || indices.empty() || instances.empty() || batches.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        const auto instanceBytes = instances.size() * sizeof(ObjectInstance);
        if (!create_device_local_buffer(vertices.data(), vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, impl_->objectVertexBuffer, impl_->objectVertexMemory)
            || !create_device_local_buffer(indices.data(), indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, impl_->objectIndexBuffer, impl_->objectIndexMemory)
            || !create_device_local_buffer(instances.data(), instanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, impl_->objectInstanceBuffer, impl_->objectInstanceMemory))
        {
            log_line("Vulkan: static object upload failed");
            return false;
        }

        impl_->objectBatches = batches;
        impl_->objectsReady = true;
        log_line("Vulkan: static objects uploaded");
        return true;
    }

    void VulkanRenderer::set_static_object_batches(const std::vector<ObjectBatch>& batches)
    {
        if (!impl_)
            return;
        impl_->objectBatches = batches;
        impl_->objectsReady = !impl_->objectBatches.empty()
            && impl_->objectVertexBuffer
            && impl_->objectIndexBuffer
            && impl_->objectInstanceBuffer;
    }

    void VulkanRenderer::set_animated_object_batches(const std::vector<ObjectBatch>& batches)
    {
        if (!impl_)
            return;
        impl_->animatedObjectBatches = batches;
    }

    void VulkanRenderer::set_terrain_draw_ranges(const std::vector<TerrainDrawRange>& ranges)
    {
        if (!impl_)
            return;
        impl_->terrainDrawRanges = ranges;
    }

    bool VulkanRenderer::update_static_object_instances(
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);
        if (impl_->objectInstanceBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectInstanceBuffer, nullptr);
        if (impl_->objectInstanceMemory)
            vkFreeMemory(impl_->device, impl_->objectInstanceMemory, nullptr);

        impl_->objectInstanceBuffer = {};
        impl_->objectInstanceMemory = {};
        impl_->objectBatches.clear();
        impl_->objectsReady = false;

        if (instances.empty() || batches.empty() || !impl_->objectVertexBuffer || !impl_->objectIndexBuffer)
            return false;

        const auto instanceBytes = instances.size() * sizeof(ObjectInstance);
        if (!create_device_local_buffer(instances.data(), instanceBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            impl_->objectInstanceBuffer,
            impl_->objectInstanceMemory))
        {
            log_line("Vulkan: static object instance upload failed");
            return false;
        }

        impl_->objectBatches = batches;
        impl_->objectsReady = true;
        return true;
    }

    bool VulkanRenderer::set_animated_object_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices,
        const std::vector<ObjectInstance>& instances,
        const std::vector<ObjectBatch>& batches)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);

        if (impl_->animatedObjectVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectVertexBuffer, nullptr);
        if (impl_->animatedObjectVertexMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectVertexMemory, nullptr);
        if (impl_->animatedObjectIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectIndexBuffer, nullptr);
        if (impl_->animatedObjectIndexMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectIndexMemory, nullptr);
        if (impl_->animatedObjectInstanceBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectInstanceBuffer, nullptr);
        if (impl_->animatedObjectInstanceMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectInstanceMemory, nullptr);

        impl_->animatedObjectVertexBuffer = {};
        impl_->animatedObjectVertexMemory = {};
        impl_->animatedObjectIndexBuffer = {};
        impl_->animatedObjectIndexMemory = {};
        impl_->animatedObjectInstanceBuffer = {};
        impl_->animatedObjectInstanceMemory = {};
        impl_->animatedObjectVertexBytes = 0;
        impl_->animatedObjectInstanceBytes = 0;
        impl_->animatedObjectBatches.clear();
        impl_->animatedObjectsReady = false;

        if (vertices.empty() || indices.empty() || instances.empty() || batches.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        const auto instanceBytes = instances.size() * sizeof(ObjectInstance);
        if (!create_host_buffer(vertices.data(), vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                impl_->animatedObjectVertexBuffer, impl_->animatedObjectVertexMemory)
            || !create_device_local_buffer(indices.data(), indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                impl_->animatedObjectIndexBuffer, impl_->animatedObjectIndexMemory)
            || !create_host_buffer(instances.data(), instanceBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                impl_->animatedObjectInstanceBuffer, impl_->animatedObjectInstanceMemory))
        {
            log_line("Vulkan: animated object upload failed");
            return false;
        }

        impl_->animatedObjectVertexBytes = vertexBytes;
        impl_->animatedObjectInstanceBytes = instanceBytes;
        impl_->animatedObjectBatches = batches;
        impl_->animatedObjectsReady = true;
        return true;
    }

    bool VulkanRenderer::update_animated_object_scene(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<ObjectInstance>& instances)
    {
        if (!impl_ || !impl_->animatedObjectsReady)
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto instanceBytes = instances.size() * sizeof(ObjectInstance);
        if (vertexBytes > impl_->animatedObjectVertexBytes || instanceBytes > impl_->animatedObjectInstanceBytes)
            return false;

        void* mapped = nullptr;
        if (vertexBytes > 0)
        {
            if (vkMapMemory(impl_->device, impl_->animatedObjectVertexMemory, 0, vertexBytes, 0, &mapped) != VK_SUCCESS)
                return false;
            std::memcpy(mapped, vertices.data(), vertexBytes);
            vkUnmapMemory(impl_->device, impl_->animatedObjectVertexMemory);
        }

        if (instanceBytes > 0)
        {
            if (vkMapMemory(impl_->device, impl_->animatedObjectInstanceMemory, 0, instanceBytes, 0, &mapped) != VK_SUCCESS)
                return false;
            std::memcpy(mapped, instances.data(), instanceBytes);
            vkUnmapMemory(impl_->device, impl_->animatedObjectInstanceMemory);
        }
        return true;
    }

    bool VulkanRenderer::set_debug_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);

        if (impl_->debugVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->debugVertexBuffer, nullptr);
        if (impl_->debugVertexMemory)
            vkFreeMemory(impl_->device, impl_->debugVertexMemory, nullptr);
        if (impl_->debugIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->debugIndexBuffer, nullptr);
        if (impl_->debugIndexMemory)
            vkFreeMemory(impl_->device, impl_->debugIndexMemory, nullptr);

        impl_->debugVertexBuffer = {};
        impl_->debugVertexMemory = {};
        impl_->debugIndexBuffer = {};
        impl_->debugIndexMemory = {};
        impl_->debugIndexCount = 0;
        impl_->debugReady = false;

        if (vertices.empty() || indices.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);
        if (!create_device_local_buffer(vertices.data(), vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, impl_->debugVertexBuffer, impl_->debugVertexMemory)
            || !create_device_local_buffer(indices.data(), indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, impl_->debugIndexBuffer, impl_->debugIndexMemory))
        {
            log_line("Vulkan: debug mesh upload failed");
            return false;
        }

        impl_->debugIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->debugReady = true;
        // (omit per-frame log to avoid spam)
        return true;
    }

    void VulkanRenderer::set_debug_visible(bool visible)
    {
        if (impl_)
            impl_->debugVisible = visible;
    }

    bool VulkanRenderer::set_character_mesh(
        const std::vector<TerrainVertex>& vertices,
        const std::vector<std::uint32_t>& indices)
    {
        if (!ready_)
            return false;

        vkDeviceWaitIdle(impl_->device);

        if (impl_->characterVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->characterVertexBuffer, nullptr);
        if (impl_->characterVertexMemory)
            vkFreeMemory(impl_->device, impl_->characterVertexMemory, nullptr);
        if (impl_->characterIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->characterIndexBuffer, nullptr);
        if (impl_->characterIndexMemory)
            vkFreeMemory(impl_->device, impl_->characterIndexMemory, nullptr);

        impl_->characterVertexBuffer = {};
        impl_->characterVertexMemory = {};
        impl_->characterIndexBuffer = {};
        impl_->characterIndexMemory = {};
        impl_->characterIndexCount = 0;
        impl_->characterVertexBytes = 0;
        impl_->characterReady = false;

        if (vertices.empty() || indices.empty())
            return false;

        const auto vertexBytes = vertices.size() * sizeof(TerrainVertex);
        const auto indexBytes = indices.size() * sizeof(std::uint32_t);

        // Use host-visible buffer for vertices (updated every frame for animation).
        if (!create_host_buffer(vertices.data(), vertexBytes,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            impl_->characterVertexBuffer, impl_->characterVertexMemory))
        {
            log_line("Vulkan: character vertex buffer creation failed");
            return false;
        }
        // Index buffer is static — use device-local.
        if (!create_device_local_buffer(indices.data(), indexBytes,
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            impl_->characterIndexBuffer, impl_->characterIndexMemory))
        {
            log_line("Vulkan: character index buffer creation failed");
            return false;
        }

        impl_->characterIndexCount = static_cast<std::uint32_t>(indices.size());
        impl_->characterVertexBytes = vertexBytes;
        impl_->characterReady = true;
        impl_->characterVisible = true;
        return true;
    }

    bool VulkanRenderer::update_character_vertices(const std::vector<TerrainVertex>& vertices)
    {
        if (!impl_ || !impl_->characterReady || vertices.empty())
            return false;

        const auto byteSize = vertices.size() * sizeof(TerrainVertex);
        if (byteSize > impl_->characterVertexBytes)
            return false;

        void* mapped = nullptr;
        if (vkMapMemory(impl_->device, impl_->characterVertexMemory, 0, byteSize, 0, &mapped) != VK_SUCCESS)
            return false;
        std::memcpy(mapped, vertices.data(), byteSize);
        vkUnmapMemory(impl_->device, impl_->characterVertexMemory);
        return true;
    }

    void VulkanRenderer::set_character_visible(bool visible)
    {
        if (impl_)
            impl_->characterVisible = visible;
    }

    void VulkanRenderer::set_camera(float x, float y, float z, float yaw, float pitch, float aspect, float farPlane)
    {
        if (!impl_)
            return;
        impl_->cameraConstants[0] = x;
        impl_->cameraConstants[1] = y;
        impl_->cameraConstants[2] = z;
        impl_->cameraConstants[3] = yaw;
        impl_->cameraConstants[4] = pitch;
        impl_->cameraConstants[5] = std::max(0.1f, aspect);
        impl_->cameraConstants[6] = 0.7002f;
        impl_->cameraConstants[7] = std::max(100.0f, farPlane);
    }

    void VulkanRenderer::set_sky_settings(const float* fogColor, float fogStartDistance, float fogEndDistance, bool hasWorldSky)
    {
        if (!impl_ || !fogColor)
            return;

        impl_->skyConstants[0] = std::clamp(fogColor[0], 0.0f, 1.0f);
        impl_->skyConstants[1] = std::clamp(fogColor[1], 0.0f, 1.0f);
        impl_->skyConstants[2] = std::clamp(fogColor[2], 0.0f, 1.0f);
        impl_->skyConstants[3] = hasWorldSky ? 1.0f : 0.0f;
        impl_->skyConstants[4] = std::max(1.0f, fogStartDistance);
        impl_->skyConstants[5] = std::max(impl_->skyConstants[4] + 1.0f, fogEndDistance);
    }

    void VulkanRenderer::set_sky_texture_layers(std::uint32_t skyLayer, std::uint32_t primaryCloudLayer, std::uint32_t secondaryCloudLayer)
    {
        if (!impl_)
            return;

        impl_->skyConstants[3] = (skyLayer != UINT32_MAX || primaryCloudLayer != UINT32_MAX || secondaryCloudLayer != UINT32_MAX) ? 1.0f : 0.0f;
        impl_->skyConstants[6] = skyLayer != UINT32_MAX ? static_cast<float>(skyLayer) : -1.0f;
        impl_->skyConstants[7] = primaryCloudLayer != UINT32_MAX ? static_cast<float>(primaryCloudLayer) : -1.0f;
        impl_->skyConstants[8] = secondaryCloudLayer != UINT32_MAX ? static_cast<float>(secondaryCloudLayer) : -1.0f;
    }

    void VulkanRenderer::set_sky_tuning(const float* values, std::uint32_t count)
    {
        if (!impl_ || !values)
            return;

        const auto copyCount = std::min<std::uint32_t>(count, static_cast<std::uint32_t>(std::size(impl_->skyTuning)));
        for (std::uint32_t i = 0; i < copyCount; ++i)
            impl_->skyTuning[i] = values[i];
    }

    void VulkanRenderer::set_water_layer(std::uint32_t waterLayer)
    {
        if (!impl_)
            return;
        impl_->skyConstants[9] = waterLayer != UINT32_MAX ? static_cast<float>(waterLayer) : static_cast<float>(0xFFFFFFFFu);
    }

    void VulkanRenderer::set_water_animation(std::uint32_t baseLayer, std::uint32_t frameCount, float tileSize)
    {
        if (!impl_)
            return;
        impl_->skyConstants[12] = static_cast<float>(baseLayer);
        impl_->skyConstants[13] = static_cast<float>(frameCount);
        impl_->skyConstants[15] = tileSize;
    }

    void VulkanRenderer::update_water_time(float totalTime)
    {
        if (!impl_)
            return;
        impl_->skyConstants[14] = totalTime;
    }

    bool VulkanRenderer::upload_terrain_texture_map(
        const std::vector<std::uint8_t>& data,
        std::uint32_t side,
        float mapSize,
        const float* tileSizes,
        std::uint32_t tileSizeCount)
    {
        if (!impl_ || data.empty() || side == 0)
            return false;

        vkDeviceWaitIdle(impl_->device);
        if (impl_->terrainMapBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainMapBuffer, nullptr);
        if (impl_->terrainMapMemory)
            vkFreeMemory(impl_->device, impl_->terrainMapMemory, nullptr);
        impl_->terrainMapBuffer = {};
        impl_->terrainMapMemory = {};
        impl_->terrainMapReady = false;

        constexpr std::uint32_t kMaxTileSizes = 16;
        const auto mapBytes = data.size();
        const auto mapBytesPadded = (mapBytes + 3u) & ~3u;
        const auto tileSizeBytes = kMaxTileSizes * sizeof(float);
        const auto totalBytes = mapBytesPadded + tileSizeBytes;

        std::vector<std::uint8_t> buffer(totalBytes, 0);
        std::memcpy(buffer.data(), data.data(), mapBytes);

        auto* tileSizeDst = reinterpret_cast<float*>(buffer.data() + mapBytesPadded);
        for (std::uint32_t i = 0; i < kMaxTileSizes; ++i)
            tileSizeDst[i] = (i < tileSizeCount && tileSizes) ? std::max(1.0f, tileSizes[i]) : 8.0f;

        if (!create_host_buffer(buffer.data(), totalBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            impl_->terrainMapBuffer, impl_->terrainMapMemory))
            return false;

        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = impl_->terrainMapBuffer;
        bufInfo.offset = 0;
        bufInfo.range = totalBytes;

        VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        write.dstSet = impl_->descriptorSet;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(impl_->device, 1, &write, 0, nullptr);

        impl_->skyConstants[10] = mapSize;
        impl_->skyConstants[11] = static_cast<float>(side);
        impl_->terrainMapReady = true;
        return true;
    }

    void VulkanRenderer::render_frame()
    {
        if (!ready_)
            return;
        if (!impl_->swapchain || impl_->swapchainExtent.width == 0 || impl_->swapchainExtent.height == 0)
            return;

        const auto frame = frameIndex_ % kMaxFramesInFlight;
        vkWaitForFences(impl_->device, 1, &impl_->inFlight[frame], VK_TRUE, UINT64_MAX);

        std::uint32_t imageIndex{};
        const auto acquire = vkAcquireNextImageKHR(
            impl_->device,
            impl_->swapchain,
            UINT64_MAX,
            impl_->imageAvailable[frame],
            VK_NULL_HANDLE,
            &imageIndex);
        if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
            return;
        if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
            return;
        vkResetFences(impl_->device, 1, &impl_->inFlight[frame]);

        const auto commandBuffer = impl_->commandBuffers[frame];
        vkResetCommandBuffer(commandBuffer, 0);

        VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &beginInfo);

        const bool hasScene = impl_->terrainReady || impl_->objectsReady;
        if (hasScene)
        {
            VkClearValue clears[2]{};
            clears[0].color.float32[0] = 0.12f;
            clears[0].color.float32[1] = 0.12f;
            clears[0].color.float32[2] = 0.14f;
            clears[0].color.float32[3] = 1.0f;
            clears[1].depthStencil.depth = 1.0f;

            VkRenderPassBeginInfo renderPassInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
            renderPassInfo.renderPass = impl_->renderPass;
            renderPassInfo.framebuffer = impl_->framebuffers[imageIndex];
            renderPassInfo.renderArea.extent = impl_->swapchainExtent;
            renderPassInfo.clearValueCount = static_cast<std::uint32_t>(std::size(clears));
            renderPassInfo.pClearValues = clears;

            vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            VkViewport viewport{};
            viewport.width = static_cast<float>(impl_->swapchainExtent.width);
            viewport.height = static_cast<float>(impl_->swapchainExtent.height);
            viewport.maxDepth = 1.0f;
            VkRect2D scissor{};
            scissor.extent = impl_->swapchainExtent;
            vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
            vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

            float constants[36]{};
            std::memcpy(constants, impl_->cameraConstants, sizeof(impl_->cameraConstants));
            std::memcpy(constants + 8, impl_->skyConstants, sizeof(impl_->skyConstants));
            std::memcpy(constants + 24, impl_->skyTuning, sizeof(impl_->skyTuning));
            constexpr auto kPushStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            if (impl_->terrainTexturesReady)
            {
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    impl_->terrainPipelineLayout, 0, 1, &impl_->descriptorSet, 0, nullptr);
            }

            if (impl_->skyPipeline)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->skyPipeline);
                vkCmdPushConstants(commandBuffer, impl_->skyPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                vkCmdDraw(commandBuffer, 3, 1, 0, 0);
            }

            if (impl_->terrainReady)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->terrainPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);

                VkDeviceSize offset{};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &impl_->terrainVertexBuffer, &offset);
                vkCmdBindIndexBuffer(commandBuffer, impl_->terrainIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                if (impl_->terrainDrawRanges.empty())
                {
                    vkCmdDrawIndexed(commandBuffer, impl_->terrainIndexCount, 1, 0, 0, 0);
                }
                else
                {
                    for (const auto& range : impl_->terrainDrawRanges)
                    {
                        if (range.indexCount > 0)
                            vkCmdDrawIndexed(commandBuffer, range.indexCount, 1, range.firstIndex, 0, 0);
                    }
                }
            }

            if (impl_->objectsReady && impl_->staticObjectPipeline)
            {
                VkBuffer buffers[2]{ impl_->objectVertexBuffer, impl_->objectInstanceBuffer };
                VkDeviceSize offsets[2]{ 0, 0 };
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->staticObjectPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                vkCmdBindVertexBuffers(commandBuffer, 0, 2, buffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, impl_->objectIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                for (const auto& batch : impl_->objectBatches)
                {
                    vkCmdDrawIndexed(commandBuffer, batch.indexCount, batch.instanceCount, batch.firstIndex, 0, batch.firstInstance);
                }
            }
            if (impl_->animatedObjectsReady && impl_->staticObjectPipeline)
            {
                VkBuffer buffers[2]{ impl_->animatedObjectVertexBuffer, impl_->animatedObjectInstanceBuffer };
                VkDeviceSize offsets[2]{ 0, 0 };
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->staticObjectPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                vkCmdBindVertexBuffers(commandBuffer, 0, 2, buffers, offsets);
                vkCmdBindIndexBuffer(commandBuffer, impl_->animatedObjectIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                for (const auto& batch : impl_->animatedObjectBatches)
                    vkCmdDrawIndexed(commandBuffer, batch.indexCount, batch.instanceCount, batch.firstIndex, 0, batch.firstInstance);
            }
            if (impl_->debugVisible && impl_->debugReady)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->terrainPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                VkDeviceSize debugOffset{};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &impl_->debugVertexBuffer, &debugOffset);
                vkCmdBindIndexBuffer(commandBuffer, impl_->debugIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, impl_->debugIndexCount, 1, 0, 0, 0);
            }
            if (impl_->characterVisible && impl_->characterReady)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->terrainPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                VkDeviceSize charOffset{};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &impl_->characterVertexBuffer, &charOffset);
                vkCmdBindIndexBuffer(commandBuffer, impl_->characterIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, impl_->characterIndexCount, 1, 0, 0, 0);
            }
            if (impl_->terrainReady && impl_->waterDrawRange.indexCount > 0)
            {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->terrainPipeline);
                vkCmdPushConstants(commandBuffer, impl_->terrainPipelineLayout,
                    kPushStages, 0, sizeof(constants), constants);
                VkDeviceSize offset{};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, &impl_->terrainVertexBuffer, &offset);
                vkCmdBindIndexBuffer(commandBuffer, impl_->terrainIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(commandBuffer, impl_->waterDrawRange.indexCount, 1, impl_->waterDrawRange.firstIndex, 0, 0);
            }
            if (imguiReady_ && imguiFrameStarted_)
            {
                ImGui::Render();
                ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
                imguiFrameStarted_ = false;
            }
            vkCmdEndRenderPass(commandBuffer);
        }
        else
        {
        VkImageMemoryBarrier toTransfer{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toTransfer.srcAccessMask = 0;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = impl_->swapchainImages[imageIndex];
        toTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toTransfer.subresourceRange.levelCount = 1;
        toTransfer.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toTransfer);

        if (impl_->previewReady)
        {
            VkImageMemoryBarrier previewToCopy{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            previewToCopy.srcAccessMask = 0;
            previewToCopy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            previewToCopy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            previewToCopy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            previewToCopy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            previewToCopy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            previewToCopy.image = impl_->previewImage;
            previewToCopy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            previewToCopy.subresourceRange.levelCount = 1;
            previewToCopy.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &previewToCopy);

            VkBufferImageCopy copy{};
            copy.bufferOffset = 0;
            copy.bufferRowLength = impl_->previewWidth;
            copy.bufferImageHeight = impl_->previewHeight;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = { impl_->previewWidth, impl_->previewHeight, 1 };
            vkCmdCopyBufferToImage(
                commandBuffer,
                impl_->previewBuffer,
                impl_->previewImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &copy);

            VkImageMemoryBarrier previewToBlit{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            previewToBlit.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            previewToBlit.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            previewToBlit.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            previewToBlit.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            previewToBlit.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            previewToBlit.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            previewToBlit.image = impl_->previewImage;
            previewToBlit.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            previewToBlit.subresourceRange.levelCount = 1;
            previewToBlit.subresourceRange.layerCount = 1;
            vkCmdPipelineBarrier(
                commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &previewToBlit);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[1] = {
                static_cast<std::int32_t>(impl_->previewWidth),
                static_cast<std::int32_t>(impl_->previewHeight),
                1,
            };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[1] = {
                static_cast<std::int32_t>(impl_->swapchainExtent.width),
                static_cast<std::int32_t>(impl_->swapchainExtent.height),
                1,
            };
            vkCmdBlitImage(
                commandBuffer,
                impl_->previewImage,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                impl_->swapchainImages[imageIndex],
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit,
                VK_FILTER_LINEAR);
        }
        else
        {
            VkClearColorValue clearColor{};
            clearColor.float32[0] = 0.09f;
            clearColor.float32[1] = 0.11f;
            clearColor.float32[2] = 0.13f;
            clearColor.float32[3] = 1.0f;
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = 1;
            range.layerCount = 1;
            vkCmdClearColorImage(commandBuffer, impl_->swapchainImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);
        }

        VkImageMemoryBarrier toPresent{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = impl_->swapchainImages[imageIndex];
        toPresent.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toPresent.subresourceRange.levelCount = 1;
        toPresent.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(
            commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 1, &toPresent);
        }
        vkEndCommandBuffer(commandBuffer);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &impl_->imageAvailable[frame];
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &impl_->renderFinished[frame];
        vkQueueSubmit(impl_->graphicsQueue, 1, &submitInfo, impl_->inFlight[frame]);

        VkPresentInfoKHR presentInfo{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &impl_->renderFinished[frame];
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &impl_->swapchain;
        presentInfo.pImageIndices = &imageIndex;
        vkQueuePresentKHR(impl_->graphicsQueue, &presentInfo);
        ++frameIndex_;
    }

    void VulkanRenderer::destroy_swapchain()
    {
        if (!impl_ || !impl_->device)
            return;
        for (auto framebuffer : impl_->framebuffers)
            vkDestroyFramebuffer(impl_->device, framebuffer, nullptr);
        impl_->framebuffers.clear();
        for (auto view : impl_->swapchainImageViews)
            vkDestroyImageView(impl_->device, view, nullptr);
        impl_->swapchainImageViews.clear();
        if (impl_->depthView)
            vkDestroyImageView(impl_->device, impl_->depthView, nullptr);
        if (impl_->depthImage)
            vkDestroyImage(impl_->device, impl_->depthImage, nullptr);
        if (impl_->depthMemory)
            vkFreeMemory(impl_->device, impl_->depthMemory, nullptr);
        impl_->depthView = {};
        impl_->depthImage = {};
        impl_->depthMemory = {};
        if (impl_->swapchain)
            vkDestroySwapchainKHR(impl_->device, impl_->swapchain, nullptr);
        impl_->swapchain = {};
    }

    void VulkanRenderer::shutdown()
    {
        if (!impl_)
            return;
        ready_ = false;
        if (impl_->device)
            vkDeviceWaitIdle(impl_->device);
        if (imguiReady_)
        {
            ImGui_ImplVulkan_Shutdown();
            ImGui_ImplSDL2_Shutdown();
            ImGui::DestroyContext();
            imguiReady_ = false;
            imguiFrameStarted_ = false;
        }

        for (std::uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            if (impl_->imageAvailable[i])
                vkDestroySemaphore(impl_->device, impl_->imageAvailable[i], nullptr);
            if (impl_->renderFinished[i])
                vkDestroySemaphore(impl_->device, impl_->renderFinished[i], nullptr);
            if (impl_->inFlight[i])
                vkDestroyFence(impl_->device, impl_->inFlight[i], nullptr);
        }
        if (impl_->previewBuffer)
            vkDestroyBuffer(impl_->device, impl_->previewBuffer, nullptr);
        if (impl_->previewMemory)
            vkFreeMemory(impl_->device, impl_->previewMemory, nullptr);
        if (impl_->previewImage)
            vkDestroyImage(impl_->device, impl_->previewImage, nullptr);
        if (impl_->previewImageMemory)
            vkFreeMemory(impl_->device, impl_->previewImageMemory, nullptr);
        if (impl_->terrainVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainVertexBuffer, nullptr);
        if (impl_->terrainVertexMemory)
            vkFreeMemory(impl_->device, impl_->terrainVertexMemory, nullptr);
        if (impl_->terrainIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainIndexBuffer, nullptr);
        if (impl_->terrainIndexMemory)
            vkFreeMemory(impl_->device, impl_->terrainIndexMemory, nullptr);
        if (impl_->objectVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectVertexBuffer, nullptr);
        if (impl_->objectVertexMemory)
            vkFreeMemory(impl_->device, impl_->objectVertexMemory, nullptr);
        if (impl_->objectIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectIndexBuffer, nullptr);
        if (impl_->objectIndexMemory)
            vkFreeMemory(impl_->device, impl_->objectIndexMemory, nullptr);
        if (impl_->objectInstanceBuffer)
            vkDestroyBuffer(impl_->device, impl_->objectInstanceBuffer, nullptr);
        if (impl_->objectInstanceMemory)
            vkFreeMemory(impl_->device, impl_->objectInstanceMemory, nullptr);
        if (impl_->animatedObjectVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectVertexBuffer, nullptr);
        if (impl_->animatedObjectVertexMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectVertexMemory, nullptr);
        if (impl_->animatedObjectIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectIndexBuffer, nullptr);
        if (impl_->animatedObjectIndexMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectIndexMemory, nullptr);
        if (impl_->animatedObjectInstanceBuffer)
            vkDestroyBuffer(impl_->device, impl_->animatedObjectInstanceBuffer, nullptr);
        if (impl_->animatedObjectInstanceMemory)
            vkFreeMemory(impl_->device, impl_->animatedObjectInstanceMemory, nullptr);
        if (impl_->debugVertexBuffer)
            vkDestroyBuffer(impl_->device, impl_->debugVertexBuffer, nullptr);
        if (impl_->debugVertexMemory)
            vkFreeMemory(impl_->device, impl_->debugVertexMemory, nullptr);
        if (impl_->debugIndexBuffer)
            vkDestroyBuffer(impl_->device, impl_->debugIndexBuffer, nullptr);
        if (impl_->debugIndexMemory)
            vkFreeMemory(impl_->device, impl_->debugIndexMemory, nullptr);
        if (impl_->terrainTextureArrayView)
            vkDestroyImageView(impl_->device, impl_->terrainTextureArrayView, nullptr);
        if (impl_->terrainTextureArray)
            vkDestroyImage(impl_->device, impl_->terrainTextureArray, nullptr);
        if (impl_->terrainTextureArrayMemory)
            vkFreeMemory(impl_->device, impl_->terrainTextureArrayMemory, nullptr);
        if (impl_->terrainMapBuffer)
            vkDestroyBuffer(impl_->device, impl_->terrainMapBuffer, nullptr);
        if (impl_->terrainMapMemory)
            vkFreeMemory(impl_->device, impl_->terrainMapMemory, nullptr);
        if (impl_->terrainSampler)
            vkDestroySampler(impl_->device, impl_->terrainSampler, nullptr);
        if (impl_->descriptorPool)
            vkDestroyDescriptorPool(impl_->device, impl_->descriptorPool, nullptr);
        if (impl_->descriptorSetLayout)
            vkDestroyDescriptorSetLayout(impl_->device, impl_->descriptorSetLayout, nullptr);
        if (impl_->terrainPipeline)
            vkDestroyPipeline(impl_->device, impl_->terrainPipeline, nullptr);
        if (impl_->staticObjectPipeline)
            vkDestroyPipeline(impl_->device, impl_->staticObjectPipeline, nullptr);
        if (impl_->terrainPipelineLayout)
            vkDestroyPipelineLayout(impl_->device, impl_->terrainPipelineLayout, nullptr);
        if (impl_->skyPipeline)
            vkDestroyPipeline(impl_->device, impl_->skyPipeline, nullptr);
        if (impl_->skyPipelineLayout)
            vkDestroyPipelineLayout(impl_->device, impl_->skyPipelineLayout, nullptr);
        if (impl_->pipelineCache)
            vkDestroyPipelineCache(impl_->device, impl_->pipelineCache, nullptr);
        if (impl_->commandPool)
            vkDestroyCommandPool(impl_->device, impl_->commandPool, nullptr);
        destroy_swapchain();
        if (impl_->renderPass)
            vkDestroyRenderPass(impl_->device, impl_->renderPass, nullptr);
        if (impl_->device)
            vkDestroyDevice(impl_->device, nullptr);
        if (impl_->surface)
            vkDestroySurfaceKHR(impl_->instance, impl_->surface, nullptr);
        if (impl_->instance)
            vkDestroyInstance(impl_->instance, nullptr);
        delete impl_;
        impl_ = nullptr;
    }
}
