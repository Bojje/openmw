#ifdef OPENMW_USE_VULKAN

#include "vklandcomposite.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <components/debug/debuglog.hpp>
#include <components/vk/vkcommands.hpp>
#include <components/vk/vkdevice.hpp>
#include <components/vk/vkshader.hpp>
#include <components/vk/vktexture.hpp>

#include "vkterrainbuilder.hpp"

namespace
{
    // Enough for any real cell. A 17x17 sample grid could in principle name 289 distinct textures,
    // but Morrowind paints from a 16x16 tile grid and a cell with more than a handful of land
    // textures does not exist in the shipping data. Anything past this is dropped with a warning
    // rather than growing the pool, because the alternative is an unbounded allocation driven by
    // content.
    constexpr uint32_t sMaxLayers = 32;

    void check(VkResult result, const char* what)
    {
        if (result != VK_SUCCESS)
            throw std::runtime_error(std::string("LandComposite: ") + what);
    }
}

namespace MWRender
{
    struct LandComposite::Impl
    {
        Vk::Device& device;
        Vk::CommandPool& commandPool;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        // Two samplers because the two bindings want opposite wrap modes, and getting them the wrong
        // way round is subtle: a repeating blend map wraps one layer's weights onto another's at the
        // cell edge, and a clamping diffuse stops tiling and stretches one texel across the cell.
        VkSampler tileSampler = VK_NULL_HANDLE;
        VkSampler clampSampler = VK_NULL_HANDLE;

        Impl(Vk::Device& d, Vk::CommandPool& p)
            : device(d)
            , commandPool(p)
        {
        }

        ~Impl()
        {
            VkDevice dev = device.handle();
            if (pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(dev, pipeline, nullptr);
            if (pipelineLayout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(dev, pipelineLayout, nullptr);
            if (descriptorPool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(dev, descriptorPool, nullptr);
            if (setLayout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
            if (renderPass != VK_NULL_HANDLE)
                vkDestroyRenderPass(dev, renderPass, nullptr);
            if (tileSampler != VK_NULL_HANDLE)
                vkDestroySampler(dev, tileSampler, nullptr);
            if (clampSampler != VK_NULL_HANDLE)
                vkDestroySampler(dev, clampSampler, nullptr);
        }
    };

    LandComposite::LandComposite(
        Vk::Device& device, Vk::CommandPool& commandPool, const std::string& shaderDir)
        : mImpl(std::make_unique<Impl>(device, commandPool))
    {
        VkDevice dev = device.handle();

        // Render pass: one colour attachment, no depth, cleared to black and left where a sampler can
        // read it. Black is load-bearing rather than tidy -- the layers accumulate additively, so the
        // clear value is the zeroth term of the sum.
        {
            VkAttachmentDescription colour = {};
            colour.format = VK_FORMAT_R8G8B8A8_SRGB;
            colour.samples = VK_SAMPLE_COUNT_1_BIT;
            colour.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colour.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colour.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colour.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colour.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkAttachmentReference colourRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colourRef;

            VkSubpassDependency dependency = {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = 1;
            info.pAttachments = &colour;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;

            check(vkCreateRenderPass(dev, &info, nullptr, &mImpl->renderPass), "render pass");
        }

        // Samplers.
        {
            VkSamplerCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            info.magFilter = VK_FILTER_LINEAR;
            info.minFilter = VK_FILTER_LINEAR;
            info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            info.maxLod = VK_LOD_CLAMP_NONE;

            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            check(vkCreateSampler(dev, &info, nullptr, &mImpl->tileSampler), "tile sampler");

            // The blend UV reaches exactly the outermost texel centres, so an epsilon of overshoot at
            // a cell edge lands outside. Clamping duplicates the edge texel; repeating would fetch a
            // texel belonging to a different layer and put a one-pixel stripe of the wrong ground
            // along every cell seam.
            info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            check(vkCreateSampler(dev, &info, nullptr, &mImpl->clampSampler), "clamp sampler");
        }

        // Descriptor set layout: the layer's diffuse, and the layer's weights.
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings = {};
            for (uint32_t i = 0; i < bindings.size(); ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            }

            VkDescriptorSetLayoutCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();

            check(vkCreateDescriptorSetLayout(dev, &info, nullptr, &mImpl->setLayout), "set layout");
        }

        // One set per layer per bake. The pool is reset at the start of every bake rather than grown,
        // because a bake is submitted and waited on before the next one starts, so nothing outlives it.
        {
            VkDescriptorPoolSize size = {};
            size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            size.descriptorCount = sMaxLayers * 2;

            VkDescriptorPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            info.maxSets = sMaxLayers;
            info.poolSizeCount = 1;
            info.pPoolSizes = &size;

            check(vkCreateDescriptorPool(dev, &info, nullptr, &mImpl->descriptorPool), "descriptor pool");
        }

        {
            VkPipelineLayoutCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            info.setLayoutCount = 1;
            info.pSetLayouts = &mImpl->setLayout;
            check(vkCreatePipelineLayout(dev, &info, nullptr, &mImpl->pipelineLayout), "pipeline layout");
        }

        // Pipeline. No vertex input at all -- landcomposite.vert derives its position from
        // gl_VertexIndex, the same way composite.vert does.
        {
            Vk::ShaderModule vert = Vk::ShaderModule::fromFile(device, shaderDir + "/landcomposite.vert.spv");
            Vk::ShaderModule frag = Vk::ShaderModule::fromFile(device, shaderDir + "/landcomposite.frag.spv");

            std::array<VkPipelineShaderStageCreateInfo, 2> stages
                = { vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT), frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT) };

            VkPipelineVertexInputStateCreateInfo vertexInput = {};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkViewport viewport = {};
            viewport.width = static_cast<float>(sSize);
            viewport.height = static_cast<float>(sSize);
            viewport.maxDepth = 1.0f;

            VkRect2D scissor = {};
            scissor.extent = { sSize, sSize };

            VkPipelineViewportStateCreateInfo viewportState = {};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.pViewports = &viewport;
            viewportState.scissorCount = 1;
            viewportState.pScissors = &scissor;

            VkPipelineRasterizationStateCreateInfo rasterizer = {};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            // No culling: the full-target triangle's winding is whatever gl_VertexIndex produced, and
            // culling it by accident yields an empty composite rather than an error.
            rasterizer.cullMode = VK_CULL_MODE_NONE;

            VkPipelineMultisampleStateCreateInfo multisampling = {};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            // The weighted sum. src.rgb * src.a added to whatever is already there, which over the
            // whole layer set gives sum(colour * weight) because the weights sum to 1. Alpha is
            // accumulated too and ends at 1 for the same reason, which is what the terrain shader
            // wants since it discards on albedo.a < 0.5.
            VkPipelineColorBlendAttachmentState blend = {};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending = {};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &blend;

            VkGraphicsPipelineCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            info.stageCount = static_cast<uint32_t>(stages.size());
            info.pStages = stages.data();
            info.pVertexInputState = &vertexInput;
            info.pInputAssemblyState = &inputAssembly;
            info.pViewportState = &viewportState;
            info.pRasterizationState = &rasterizer;
            info.pMultisampleState = &multisampling;
            info.pColorBlendState = &colorBlending;
            info.layout = mImpl->pipelineLayout;
            info.renderPass = mImpl->renderPass;
            info.subpass = 0;

            check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &info, nullptr, &mImpl->pipeline),
                "pipeline");
        }
    }

    LandComposite::~LandComposite() = default;

    Vk::Texture LandComposite::bake(const LandBlend& blend, const std::vector<VkImageView>& layerViews)
    {
        if (blend.layers.size() < 2 || blend.maps.size() != blend.layers.size()
            || layerViews.size() != blend.layers.size())
            return {};

        uint32_t layerCount = static_cast<uint32_t>(blend.layers.size());
        if (layerCount > sMaxLayers)
        {
            Log(Debug::Warning) << "Vulkan: cell has " << layerCount << " land textures, compositing the first "
                                << sMaxLayers;
            layerCount = sMaxLayers;
        }

        VkDevice dev = mImpl->device.handle();

        // The weight maps. Uploaded per bake and destroyed with this vector when it returns -- they
        // are 34x34 single-channel and only the bake ever reads them, so keeping them resident would
        // cost sampler slots for nothing.
        std::vector<Vk::Texture> weightTextures;
        weightTextures.reserve(layerCount);
        for (uint32_t i = 0; i < layerCount; ++i)
        {
            const std::vector<std::uint8_t>& map = blend.maps[i];
            weightTextures.push_back(Vk::Texture::create(mImpl->device, mImpl->commandPool, sBlendImageSize,
                sBlendImageSize, VK_FORMAT_R8_UNORM, map.data(), static_cast<VkDeviceSize>(map.size())));
        }

        Vk::Texture target = Vk::Texture::createRenderTarget(
            mImpl->device, sSize, sSize, VK_FORMAT_R8G8B8A8_SRGB, 32);

        VkImageView targetView = target.view();

        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        {
            VkFramebufferCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            info.renderPass = mImpl->renderPass;
            info.attachmentCount = 1;
            info.pAttachments = &targetView;
            info.width = sSize;
            info.height = sSize;
            info.layers = 1;
            check(vkCreateFramebuffer(dev, &info, nullptr, &framebuffer), "framebuffer");
        }

        // Safe because a bake is submitted and waited on before the next begins, so no set from a
        // previous bake is still in use.
        check(vkResetDescriptorPool(dev, mImpl->descriptorPool, 0), "reset descriptor pool");

        std::vector<VkDescriptorSetLayout> layouts(layerCount, mImpl->setLayout);
        std::vector<VkDescriptorSet> sets(layerCount, VK_NULL_HANDLE);
        {
            VkDescriptorSetAllocateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            info.descriptorPool = mImpl->descriptorPool;
            info.descriptorSetCount = layerCount;
            info.pSetLayouts = layouts.data();
            check(vkAllocateDescriptorSets(dev, &info, sets.data()), "descriptor sets");
        }

        for (uint32_t i = 0; i < layerCount; ++i)
        {
            VkDescriptorImageInfo diffuse = {};
            diffuse.sampler = mImpl->tileSampler;
            diffuse.imageView = layerViews[i];
            diffuse.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkDescriptorImageInfo weights = {};
            weights.sampler = mImpl->clampSampler;
            weights.imageView = weightTextures[i].view();
            weights.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            std::array<VkWriteDescriptorSet, 2> writes = {};
            for (uint32_t w = 0; w < writes.size(); ++w)
            {
                writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[w].dstSet = sets[i];
                writes[w].dstBinding = w;
                writes[w].descriptorCount = 1;
                writes[w].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            }
            writes[0].pImageInfo = &diffuse;
            writes[1].pImageInfo = &weights;

            vkUpdateDescriptorSets(dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        VkCommandBuffer cmd = mImpl->commandPool.beginSingleTime();

        VkClearValue clear = {};
        clear.color = { { 0.0f, 0.0f, 0.0f, 0.0f } };

        VkRenderPassBeginInfo pass = {};
        pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        pass.renderPass = mImpl->renderPass;
        pass.framebuffer = framebuffer;
        pass.renderArea.extent = { sSize, sSize };
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;

        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mImpl->pipeline);
        for (uint32_t i = 0; i < layerCount; ++i)
        {
            vkCmdBindDescriptorSets(
                cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mImpl->pipelineLayout, 0, 1, &sets[i], 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);

        mImpl->commandPool.endSingleTime(cmd, mImpl->device.graphicsQueue());

        // After the pass, because the render pass leaves level 0 in SHADER_READ_ONLY_OPTIMAL, which is
        // where generateMipChain expects to find it.
        target.generateMipChain(mImpl->device, mImpl->commandPool);

        vkDestroyFramebuffer(dev, framebuffer, nullptr);

        return target;
    }
}

#endif
