// Procedural weapon-effect particle pipeline (textureless soft billboards).
// Split out of vulkan_renderer.cpp; shares VulkanRenderer::Impl via the internal
// header. See shaders/particle.hlsl.

#include "renderer/vulkan_renderer_internal.h"

#include <algorithm>
#include <cstring>

namespace phoenix::renderer
{
    bool VulkanRenderer::create_particle_pipeline()
    {
        VkShaderModule vertexShader{};
        VkShaderModule fragmentShader{};
        if (!load_shader_module("shaders/compiled/particle.vert.spv", vertexShader)
            || !load_shader_module("shaders/compiled/particle.frag.spv", fragmentShader))
        {
            log_line("Vulkan: particle shaders not found, weapon effects disabled");
            return true; // non-fatal
        }

        // Dedicated descriptor layout: binding 0 = per-particle instance storage
        // buffer (vertex). Particles are drawn with a fully procedural soft sprite
        // (no textures). Separate from the terrain set so terrain is never touched.
        VkDescriptorSetLayoutBinding bindings[1]{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(impl_->device, &layoutInfo, nullptr, &impl_->particleSetLayout) != VK_SUCCESS)
        {
            vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
            vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
            return false;
        }

        VkDescriptorPoolSize poolSizes[1]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[0].descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = poolSizes;
        if (vkCreateDescriptorPool(impl_->device, &poolInfo, nullptr, &impl_->particleDescriptorPool) != VK_SUCCESS)
        {
            vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
            vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
            return false;
        }

        VkDescriptorSetAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        allocInfo.descriptorPool = impl_->particleDescriptorPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &impl_->particleSetLayout;
        if (vkAllocateDescriptorSets(impl_->device, &allocInfo, &impl_->particleDescriptorSet) != VK_SUCCESS)
        {
            vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
            vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
            return false;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(float) * 40;

        VkPipelineLayoutCreateInfo pipeLayoutInfo{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeLayoutInfo.setLayoutCount = 1;
        pipeLayoutInfo.pSetLayouts = &impl_->particleSetLayout;
        pipeLayoutInfo.pushConstantRangeCount = 1;
        pipeLayoutInfo.pPushConstantRanges = &pushRange;
        if (vkCreatePipelineLayout(impl_->device, &pipeLayoutInfo, nullptr, &impl_->particlePipelineLayout) != VK_SUCCESS)
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

        // Particles test against scene depth but never write it (transparent overlay).
        VkPipelineDepthStencilStateCreateInfo depth{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        // Alpha-blended attachment (TextureBlendMode 0 = Normal).
        VkPipelineColorBlendAttachmentState alphaAttachment{};
        alphaAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        alphaAttachment.blendEnable = VK_TRUE;
        alphaAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        alphaAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        alphaAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        alphaAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        alphaAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        alphaAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo alphaBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        alphaBlend.attachmentCount = 1;
        alphaBlend.pAttachments = &alphaAttachment;

        // Additive attachment (TextureBlendMode > 0 = glow/light accumulation).
        VkPipelineColorBlendAttachmentState additiveAttachment = alphaAttachment;
        additiveAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        additiveAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        additiveAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        VkPipelineColorBlendStateCreateInfo additiveBlend{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        additiveBlend.attachmentCount = 1;
        additiveBlend.pAttachments = &additiveAttachment;

        VkGraphicsPipelineCreateInfo createInfo{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        createInfo.stageCount = 2;
        createInfo.pStages = stages;
        createInfo.pVertexInputState = &vertexInput;
        createInfo.pInputAssemblyState = &inputAssembly;
        createInfo.pViewportState = &viewportState;
        createInfo.pRasterizationState = &raster;
        createInfo.pMultisampleState = &multisample;
        createInfo.pDepthStencilState = &depth;
        createInfo.pColorBlendState = &alphaBlend;
        createInfo.pDynamicState = &dynamicState;
        createInfo.layout = impl_->particlePipelineLayout;
        createInfo.renderPass = impl_->renderPass;
        createInfo.subpass = 0;

        bool ok = vkCreateGraphicsPipelines(impl_->device, impl_->pipelineCache, 1, &createInfo, nullptr, &impl_->particlePipelineAlpha) == VK_SUCCESS;
        createInfo.pColorBlendState = &additiveBlend;
        ok = ok && vkCreateGraphicsPipelines(impl_->device, impl_->pipelineCache, 1, &createInfo, nullptr, &impl_->particlePipelineAdditive) == VK_SUCCESS;

        vkDestroyShaderModule(impl_->device, vertexShader, nullptr);
        vkDestroyShaderModule(impl_->device, fragmentShader, nullptr);
        impl_->particlePipelineReady = ok;
        return ok;
    }


    void VulkanRenderer::set_particle_instances(const std::vector<ParticleInstance>& instances, std::uint32_t additiveStart)
    {
        if (!ready_ || !impl_->particlePipelineReady)
            return;

        impl_->particleInstanceCount = static_cast<std::uint32_t>(instances.size());
        impl_->particleAdditiveStart = std::min(additiveStart, impl_->particleInstanceCount);
        if (instances.empty())
            return;

        const std::size_t byteSize = instances.size() * sizeof(ParticleInstance);

        // Grow (or first-allocate) the host-visible storage buffer when needed.
        if (byteSize > impl_->particleInstanceCapacity || !impl_->particleInstanceBuffer)
        {
            vkDeviceWaitIdle(impl_->device);
            if (impl_->particleInstanceBuffer)
                vkDestroyBuffer(impl_->device, impl_->particleInstanceBuffer, nullptr);
            if (impl_->particleInstanceMemory)
                vkFreeMemory(impl_->device, impl_->particleInstanceMemory, nullptr);
            impl_->particleInstanceBuffer = {};
            impl_->particleInstanceMemory = {};
            impl_->particleInstanceCapacity = 0;

            const std::size_t newCapacity = byteSize + byteSize / 2 + 4096;
            impl_->particleInstanceMapped = nullptr;
            if (!create_host_buffer(nullptr, newCapacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                impl_->particleInstanceBuffer, impl_->particleInstanceMemory,
                &impl_->particleInstanceMapped))
            {
                impl_->particleInstanceCount = 0;
                return;
            }
            impl_->particleInstanceCapacity = newCapacity;

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = impl_->particleInstanceBuffer;
            bufInfo.offset = 0;
            bufInfo.range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet write{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            write.dstSet = impl_->particleDescriptorSet;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(impl_->device, 1, &write, 0, nullptr);
        }

        // Persistent-mapped path: direct memcpy, no per-frame map/unmap.
        if (impl_->particleInstanceMapped)
        {
            std::memcpy(impl_->particleInstanceMapped, instances.data(), byteSize);
        }
        else
        {
            void* mapped{};
            if (vkMapMemory(impl_->device, impl_->particleInstanceMemory, 0, byteSize, 0, &mapped) == VK_SUCCESS)
            {
                std::memcpy(mapped, instances.data(), byteSize);
                vkUnmapMemory(impl_->device, impl_->particleInstanceMemory);
            }
        }
    }

}
