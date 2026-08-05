#include "vkrenderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>

#include "../render/math.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"
#include "vkinstance.hpp"
#include "vkshader.hpp"
#include "vkswapchain.hpp"
#include "vksync.hpp"

namespace Vk
{
    static uint32_t findMemoryTypeLocal(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);
        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        throw std::runtime_error("Failed to find suitable memory type");
    }

    static void createBufferLocal(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryTypeLocal(physicalDevice, memRequirements.memoryTypeBits, properties);

        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
    }

    Renderer::Renderer(SDL_Window* window, bool enableValidation)
        : mWindow(window)
    {
        mInstance = std::make_unique<Instance>("OpenMW", "OpenMW Engine", enableValidation);
        createSurface();

        mDevice = std::make_unique<Device>(*mInstance, mSurface);

        int w, h;
        SDL_Vulkan_GetDrawableSize(window, &w, &h);

        mSwapchain = std::make_unique<Swapchain>(*mDevice, mSurface,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        mCommandPool = std::make_unique<CommandPool>(*mDevice, mDevice->indices().graphics.value());
        mFrameSync = std::make_unique<FrameSync>(*mDevice, mSwapchain->imageCount());

        mCommandBuffers = mCommandPool->allocateMultiple(maxFramesInFlight);

        createGBufferRenderPass();
        createCompositeRenderPass();
        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();
        createGBufferSampler();
        createDescriptorSetLayouts();
        createDescriptorPool();
        createUniformBuffers();
        createDescriptorSets();
        createGBufferPipeline();
        createCompositePipeline();

    }

    Renderer::~Renderer()
    {
        cleanup();
    }

    void Renderer::createSurface()
    {
        if (!SDL_Vulkan_CreateSurface(mWindow, mInstance->handle(), &mSurface))
            throw std::runtime_error("Failed to create Vulkan surface via SDL");
    }

    void Renderer::createImage(uint32_t width, uint32_t height, VkFormat format,
        VkImageUsageFlags usage, VkImage& image, VkDeviceMemory& memory)
    {
        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = usage;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VK_CHECK(vkCreateImage(mDevice->handle(), &imageInfo, nullptr, &image));

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(mDevice->handle(), image, &memReqs);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = mDevice->findMemoryType(memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VK_CHECK(vkAllocateMemory(mDevice->handle(), &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindImageMemory(mDevice->handle(), image, memory, 0));
    }

    VkImageView Renderer::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags)
    {
        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView view;
        VK_CHECK(vkCreateImageView(mDevice->handle(), &viewInfo, nullptr, &view));
        return view;
    }

    void Renderer::transitionImageLayout(VkCommandBuffer cmd, VkImage image,
        VkImageLayout oldLayout, VkImageLayout newLayout, VkImageAspectFlags aspectMask)
    {
        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = aspectMask;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage;
        VkPipelineStageFlags dstStage;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else
        {
            barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            dstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }

        vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }

    VkFormat Renderer::findDepthFormat()
    {
        const std::array<VkFormat, 3> candidates = {
            VK_FORMAT_D32_SFLOAT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
            VK_FORMAT_D24_UNORM_S8_UINT
        };

        for (VkFormat format : candidates)
        {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(mDevice->physical(), format, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                return format;
        }
        throw std::runtime_error("Failed to find supported depth format");
    }

    // G-buffer

    void Renderer::createGBuffer()
    {
        VkExtent2D extent = mSwapchain->extent();

        createImage(extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.albedoImage, mGBuffer.albedoMemory);
        mGBuffer.albedoView = createImageView(mGBuffer.albedoImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

        createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.normalImage, mGBuffer.normalMemory);
        mGBuffer.normalView = createImageView(mGBuffer.normalImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

        createImage(extent.width, extent.height, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.materialImage, mGBuffer.materialMemory);
        mGBuffer.materialView = createImageView(mGBuffer.materialImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

        VkFormat depthFormat = findDepthFormat();
        createImage(extent.width, extent.height, depthFormat,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.depthImage, mGBuffer.depthMemory);
        mGBuffer.depthView = createImageView(mGBuffer.depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    void Renderer::destroyGBuffer()
    {
        VkDevice dev = mDevice->handle();

        auto destroyAttachment = [dev](VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
            if (view != VK_NULL_HANDLE) { vkDestroyImageView(dev, view, nullptr); view = VK_NULL_HANDLE; }
            if (img != VK_NULL_HANDLE) { vkDestroyImage(dev, img, nullptr); img = VK_NULL_HANDLE; }
            if (mem != VK_NULL_HANDLE) { vkFreeMemory(dev, mem, nullptr); mem = VK_NULL_HANDLE; }
        };

        destroyAttachment(mGBuffer.albedoImage, mGBuffer.albedoMemory, mGBuffer.albedoView);
        destroyAttachment(mGBuffer.normalImage, mGBuffer.normalMemory, mGBuffer.normalView);
        destroyAttachment(mGBuffer.materialImage, mGBuffer.materialMemory, mGBuffer.materialView);
        destroyAttachment(mGBuffer.depthImage, mGBuffer.depthMemory, mGBuffer.depthView);
    }

    // Render passes

    void Renderer::createGBufferRenderPass()
    {
        VkFormat depthFormat = findDepthFormat();

        std::array<VkAttachmentDescription, 4> attachments = {};

        // Albedo
        attachments[0].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Normal
        attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Material
        attachments[2].format = VK_FORMAT_R8G8B8A8_UNORM;
        attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Depth
        attachments[3].format = depthFormat;
        attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkAttachmentReference, 3> colorRefs = {};
        colorRefs[0] = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[1] = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[2] = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

        VkAttachmentReference depthRef = { 3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.data();
        subpass.pDepthStencilAttachment = &depthRef;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        VK_CHECK(vkCreateRenderPass(mDevice->handle(), &renderPassInfo, nullptr, &mGBufferRenderPass));
    }

    void Renderer::createCompositeRenderPass()
    {
        VkAttachmentDescription colorAttachment = {};
        colorAttachment.format = mSwapchain->format();
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = 1;
        renderPassInfo.pAttachments = &colorAttachment;
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        VK_CHECK(vkCreateRenderPass(mDevice->handle(), &renderPassInfo, nullptr, &mCompositeRenderPass));
    }

    // Framebuffers

    void Renderer::createGBufferFramebuffer()
    {
        VkExtent2D extent = mSwapchain->extent();
        std::array<VkImageView, 4> attachments = {
            mGBuffer.albedoView,
            mGBuffer.normalView,
            mGBuffer.materialView,
            mGBuffer.depthView
        };

        VkFramebufferCreateInfo fbInfo = {};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = mGBufferRenderPass;
        fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        fbInfo.pAttachments = attachments.data();
        fbInfo.width = extent.width;
        fbInfo.height = extent.height;
        fbInfo.layers = 1;

        VK_CHECK(vkCreateFramebuffer(mDevice->handle(), &fbInfo, nullptr, &mGBufferFramebuffer));
    }

    void Renderer::createCompositeFramebuffers()
    {
        const auto& imageViews = mSwapchain->imageViews();
        mCompositeFramebuffers.resize(imageViews.size());
        VkExtent2D extent = mSwapchain->extent();

        for (size_t i = 0; i < imageViews.size(); i++)
        {
            VkImageView attachment = imageViews[i];

            VkFramebufferCreateInfo fbInfo = {};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = mCompositeRenderPass;
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments = &attachment;
            fbInfo.width = extent.width;
            fbInfo.height = extent.height;
            fbInfo.layers = 1;

            VK_CHECK(vkCreateFramebuffer(mDevice->handle(), &fbInfo, nullptr, &mCompositeFramebuffers[i]));
        }
    }

    void Renderer::destroyCompositeFramebuffers()
    {
        for (auto fb : mCompositeFramebuffers)
            vkDestroyFramebuffer(mDevice->handle(), fb, nullptr);
        mCompositeFramebuffers.clear();
    }

    // Sampler

    void Renderer::createGBufferSampler()
    {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_NEAREST;
        samplerInfo.minFilter = VK_FILTER_NEAREST;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mGBufferSampler));
    }

    void Renderer::writeCompositeDescriptor(uint32_t binding, VkImageView view)
    {
        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.sampler = mGBufferSampler;
            imageInfo.imageView = view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = mCompositeDescriptorSets[i];
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
        }
    }

    // Descriptor set layouts

    void Renderer::createDescriptorSetLayouts()
    {
        // Scene layout (set 0 for G-buffer pass): camera UBO
        {
            VkDescriptorSetLayoutBinding uboBinding = {};
            uboBinding.binding = 0;
            uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uboBinding.descriptorCount = 1;
            uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = 1;
            layoutInfo.pBindings = &uboBinding;

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mSceneDescriptorLayout));
        }

        // Composite layout: G-buffer textures, scene UBO, and material data
        {
            std::array<VkDescriptorSetLayoutBinding, 5> bindings = {};

            // Albedo
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Normal
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Depth
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Scene UBO (view/projection inverse matrices for world position reconstruction)
            bindings[3].binding = 4;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Material (PBR parameters from G-buffer)
            bindings[4].binding = 5;
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[4].descriptorCount = 1;
            bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mCompositeDescriptorLayout));
        }

    }

    // Descriptor pool

    void Renderer::createDescriptorPool()
    {
        std::vector<VkDescriptorPoolSize> poolSizes = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxFramesInFlight * 2 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 + maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
        };

        uint32_t maxSets = maxFramesInFlight * 2 + 4;

        VkDescriptorPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = maxSets;

        VK_CHECK(vkCreateDescriptorPool(mDevice->handle(), &poolInfo, nullptr, &mDescriptorPool));
    }

    // Uniform buffers

    void Renderer::createUniformBuffers()
    {
        VkDeviceSize bufferSize = sizeof(Render::SceneData);

        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            createBufferLocal(mDevice->handle(), mDevice->physical(), bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mUniformBuffers[i], mUniformMemory[i]);

            VK_CHECK(vkMapMemory(mDevice->handle(), mUniformMemory[i], 0, bufferSize, 0, &mUniformMapped[i]));
        }
    }

    // Descriptor sets

    void Renderer::createDescriptorSets()
    {
        // Scene descriptor sets (per frame)
        {
            std::array<VkDescriptorSetLayout, maxFramesInFlight> layouts;
            layouts.fill(mSceneDescriptorLayout);

            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = mDescriptorPool;
            allocInfo.descriptorSetCount = maxFramesInFlight;
            allocInfo.pSetLayouts = layouts.data();

            VK_CHECK(vkAllocateDescriptorSets(mDevice->handle(), &allocInfo, mSceneDescriptorSets.data()));

            for (uint32_t i = 0; i < maxFramesInFlight; i++)
            {
                VkDescriptorBufferInfo bufferInfo = {};
                bufferInfo.buffer = mUniformBuffers[i];
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(Render::SceneData);

                VkWriteDescriptorSet write = {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = mSceneDescriptorSets[i];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &bufferInfo;

                vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
            }
        }

        // Composite descriptor sets (per frame, for per-frame UBO binding)
        {
            std::array<VkDescriptorSetLayout, maxFramesInFlight> compositeLayouts;
            compositeLayouts.fill(mCompositeDescriptorLayout);

            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = mDescriptorPool;
            allocInfo.descriptorSetCount = maxFramesInFlight;
            allocInfo.pSetLayouts = compositeLayouts.data();

            VK_CHECK(vkAllocateDescriptorSets(mDevice->handle(), &allocInfo, mCompositeDescriptorSets.data()));

            writeCompositeDescriptor(0, mGBuffer.albedoView);
            writeCompositeDescriptor(1, mGBuffer.normalView);
            writeCompositeDescriptor(2, mGBuffer.depthView);
            writeCompositeDescriptor(5, mGBuffer.materialView);

            // Bind the scene UBO to each per-frame composite descriptor set
            for (uint32_t i = 0; i < maxFramesInFlight; i++)
            {
                VkDescriptorBufferInfo bufferInfo = {};
                bufferInfo.buffer = mUniformBuffers[i];
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(Render::SceneData);

                VkWriteDescriptorSet write = {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = mCompositeDescriptorSets[i];
                write.dstBinding = 4;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &bufferInfo;

                vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
            }
        }
    }

    // Pipelines

    void Renderer::createGBufferPipeline()
    {
        VkPushConstantRange pushConstant = {};
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(Render::Mat4) * 2;

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &mSceneDescriptorLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;

        VK_CHECK(vkCreatePipelineLayout(mDevice->handle(), &layoutInfo, nullptr, &mGBufferPipelineLayout));
    }

    void Renderer::createCompositePipeline()
    {
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &mCompositeDescriptorLayout;

        VkPushConstantRange pushConstant = {};
        pushConstant.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(Render::Vec4) * 3;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstant;

        VK_CHECK(vkCreatePipelineLayout(mDevice->handle(), &layoutInfo, nullptr, &mCompositePipelineLayout));
    }

    bool Renderer::loadShadersAndCreatePipelines(const std::string& shaderDir)
    {
        auto loadShader = [&](const std::string& name) -> std::unique_ptr<ShaderModule> {
            std::string path = shaderDir + "/" + name;
            std::ifstream test(path, std::ios::binary);
            if (!test.good())
                return nullptr;
            test.close();
            return std::make_unique<ShaderModule>(ShaderModule::fromFile(*mDevice, path));
        };

        auto gbufVert = loadShader("gbuffer.vert.spv");
        auto gbufFrag = loadShader("gbuffer.frag.spv");
        auto compVert = loadShader("composite.vert.spv");
        auto compFrag = loadShader("composite.frag.spv");

        if (!gbufVert || !gbufFrag || !compVert || !compFrag)
        {
            Log(Debug::Warning) << "Vulkan SPIR-V shaders not found in " << shaderDir
                                << ", rendering disabled until shaders are compiled";
            return false;
        }

        std::array<VkDynamicState, 2> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };

        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        // G-buffer pipeline
        {
            std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
                gbufVert->stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                gbufFrag->stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT)
            };

            std::array<VkVertexInputBindingDescription, 1> bindingDesc = {};
            bindingDesc[0].binding = 0;
            bindingDesc[0].stride = sizeof(float) * (3 + 3 + 2 + 4);
            bindingDesc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::array<VkVertexInputAttributeDescription, 4> attrDesc = {};
            attrDesc[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
            attrDesc[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 };
            attrDesc[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6 };
            attrDesc[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 8 };

            VkPipelineVertexInputStateCreateInfo vertexInput = {};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDesc.size());
            vertexInput.pVertexBindingDescriptions = bindingDesc.data();
            vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDesc.size());
            vertexInput.pVertexAttributeDescriptions = attrDesc.data();

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewportState = {};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer = {};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

            VkPipelineMultisampleStateCreateInfo multisampling = {};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil = {};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_TRUE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;

            std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments = {};
            for (auto& att : blendAttachments)
            {
                att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                    | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                att.blendEnable = VK_FALSE;
            }

            VkPipelineColorBlendStateCreateInfo colorBlending = {};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = static_cast<uint32_t>(blendAttachments.size());
            colorBlending.pAttachments = blendAttachments.data();

            VkGraphicsPipelineCreateInfo pipelineInfo = {};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
            pipelineInfo.pStages = stages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = mGBufferPipelineLayout;
            pipelineInfo.renderPass = mGBufferRenderPass;
            pipelineInfo.subpass = 0;

            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferPipeline));
        }

        // Composite pipeline
        {
            std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
                compVert->stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                compFrag->stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT)
            };

            VkPipelineVertexInputStateCreateInfo vertexInput = {};
            vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

            VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

            VkPipelineViewportStateCreateInfo viewportState = {};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;

            VkPipelineRasterizationStateCreateInfo rasterizer = {};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_NONE;

            VkPipelineMultisampleStateCreateInfo multisampling = {};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            VkPipelineDepthStencilStateCreateInfo depthStencil = {};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_FALSE;

            VkPipelineColorBlendAttachmentState blendAttachment = {};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blendAttachment.blendEnable = VK_FALSE;

            VkPipelineColorBlendStateCreateInfo colorBlending = {};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &blendAttachment;

            VkGraphicsPipelineCreateInfo pipelineInfo = {};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
            pipelineInfo.pStages = stages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = mCompositePipelineLayout;
            pipelineInfo.renderPass = mCompositeRenderPass;
            pipelineInfo.subpass = 0;

            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mCompositePipeline));
        }

        Log(Debug::Info) << "Vulkan pipelines created successfully";
        return true;
    }

    // Frame lifecycle

    bool Renderer::beginFrame()
    {
        mFrameSync->waitForFrame(mCurrentFrame);

        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(mWindow, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
            return false;

        VkResult result;
        for (;;)
        {
            result = vkAcquireNextImageKHR(mDevice->handle(), mSwapchain->handle(),
                UINT64_MAX, mFrameSync->imageAvailable(mCurrentFrame), VK_NULL_HANDLE, &mCurrentImageIndex);

            if (result == VK_ERROR_OUT_OF_DATE_KHR)
            {
                SDL_Vulkan_GetDrawableSize(mWindow, &drawableWidth, &drawableHeight);
                if (drawableWidth <= 0 || drawableHeight <= 0)
                    return false;
                resize(static_cast<uint32_t>(drawableWidth), static_cast<uint32_t>(drawableHeight));
                continue;
            }
            break;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            VK_CHECK(result);

        mFrameSync->waitForImage(mCurrentImageIndex, mCurrentFrame);
        mFrameSync->resetFrame(mCurrentFrame);

        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

        return true;
    }

    void Renderer::endFrame()
    {
        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSemaphore waitSemaphore = mFrameSync->imageAvailable(mCurrentFrame);
        VkSemaphore signalSemaphore = mFrameSync->renderFinished(mCurrentImageIndex);
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &waitSemaphore;
        submitInfo.pWaitDstStageMask = &waitStage;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &cmd;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &signalSemaphore;

        VK_CHECK(vkQueueSubmit(mDevice->graphicsQueue(), 1, &submitInfo,
            mFrameSync->inFlightFence(mCurrentFrame)));

        VkSwapchainKHR swapchain = mSwapchain->handle();

        VkPresentInfoKHR presentInfo = {};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &signalSemaphore;
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = &swapchain;
        presentInfo.pImageIndices = &mCurrentImageIndex;

        VkResult result = vkQueuePresentKHR(mDevice->presentQueue(), &presentInfo);
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
        {
            int w, h;
            SDL_Vulkan_GetDrawableSize(mWindow, &w, &h);
            resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }

        mCurrentFrame = (mCurrentFrame + 1) % maxFramesInFlight;
    }

    bool Renderer::render()
    {
        if (!beginFrame())
            return false;

        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        VkExtent2D extent = mSwapchain->extent();

        // 1. G-buffer pass
        {
            std::array<VkClearValue, 4> clearValues = {};
            clearValues[0].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[1].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[2].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[3].depthStencil = { 1.0f, 0 };

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = mGBufferRenderPass;
            renderPassInfo.framebuffer = mGBufferFramebuffer;
            renderPassInfo.renderArea.extent = extent;
            renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
            renderPassInfo.pClearValues = clearValues.data();

            vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            if (mGBufferPipeline != VK_NULL_HANDLE)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mGBufferPipeline);

                VkViewport viewport = {};
                viewport.width = static_cast<float>(extent.width);
                viewport.height = static_cast<float>(extent.height);
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor = {};
                scissor.extent = extent;
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mGBufferPipelineLayout,
                    0, 1, &mSceneDescriptorSets[mCurrentFrame], 0, nullptr);

                if (mMeshVertexBuffer != VK_NULL_HANDLE && mMeshIndexBuffer != VK_NULL_HANDLE)
                {
                    struct PushData
                    {
                        Render::Mat4 model;
                        Render::Mat4 normalMatrix;
                    };

                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &mMeshVertexBuffer, &offset);
                    vkCmdBindIndexBuffer(cmd, mMeshIndexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    for (const Render::MeshDraw& draw : mMeshDraws)
                    {
                        const PushData pushData = { draw.transform, draw.normalMatrix };
                        vkCmdPushConstants(cmd, mGBufferPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                            0, sizeof(pushData), &pushData);
                        vkCmdDrawIndexed(cmd, draw.indexCount, 1, draw.firstIndex, draw.vertexOffset, 0);
                    }
                }

            }

            vkCmdEndRenderPass(cmd);
        }

        // 2. Composite pass
        {
            VkClearValue clearValue = {};
            clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

            VkRenderPassBeginInfo renderPassInfo = {};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = mCompositeRenderPass;
            renderPassInfo.framebuffer = mCompositeFramebuffers[mCurrentImageIndex];
            renderPassInfo.renderArea.extent = extent;
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &clearValue;

            vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

            if (mCompositePipeline != VK_NULL_HANDLE)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompositePipeline);

                VkViewport viewport = {};
                viewport.width = static_cast<float>(extent.width);
                viewport.height = static_cast<float>(extent.height);
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor = {};
                scissor.extent = extent;
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mCompositePipelineLayout,
                    0, 1, &mCompositeDescriptorSets[mCurrentFrame], 0, nullptr);

                // Push sun direction, sun color, and camera position
                const Render::SceneData* sceneData = static_cast<const Render::SceneData*>(mUniformMapped[mCurrentFrame]);
                Render::Vec4 cameraPos;
                cameraPos.x = sceneData->viewInverse.data[12];
                cameraPos.y = sceneData->viewInverse.data[13];
                cameraPos.z = sceneData->viewInverse.data[14];
                cameraPos.w = 1.0f;

                std::array<Render::Vec4, 3> compositePush = { sceneData->sunDirection, sceneData->sunColor, cameraPos };
                vkCmdPushConstants(cmd, mCompositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(Render::Vec4) * 3, compositePush.data());

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }

            vkCmdEndRenderPass(cmd);
        }

        endFrame();
        return true;
    }

    void Renderer::resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;

        vkDeviceWaitIdle(mDevice->handle());

        if (mGBufferFramebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(mDevice->handle(), mGBufferFramebuffer, nullptr);
            mGBufferFramebuffer = VK_NULL_HANDLE;
        }
        destroyCompositeFramebuffers();
        destroyGBuffer();

        mSwapchain->recreate(width, height);
        mFrameSync->resizeRenderFinished(mSwapchain->imageCount());

        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();

        writeCompositeDescriptor(0, mGBuffer.albedoView);
        writeCompositeDescriptor(1, mGBuffer.normalView);
        writeCompositeDescriptor(2, mGBuffer.depthView);
        writeCompositeDescriptor(5, mGBuffer.materialView);
    }

    void Renderer::updateScene(const Render::SceneData& sceneData)
    {
        std::memcpy(mUniformMapped[mCurrentFrame], &sceneData, sizeof(Render::SceneData));
    }

    void Renderer::setMeshes(const std::vector<Render::MeshInstance>& meshes)
    {
        vkDeviceWaitIdle(mDevice->handle());
        destroyMesh();

        Render::MeshBatch batch = Render::batchMeshes(meshes);
        std::vector<Render::MeshVertex>& vertices = batch.vertices;
        std::vector<uint32_t>& indices = batch.indices;

        if (vertices.empty() || indices.empty())
            return;

        mMeshDraws = std::move(batch.draws);

        try
        {
            createBufferLocal(mDevice->handle(), mDevice->physical(),
                sizeof(Render::MeshVertex) * vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mMeshVertexBuffer, mMeshVertexMemory);
            createBufferLocal(mDevice->handle(), mDevice->physical(),
                sizeof(uint32_t) * indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mMeshIndexBuffer, mMeshIndexMemory);

            void* mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), mMeshVertexMemory, 0,
                sizeof(Render::MeshVertex) * vertices.size(), 0, &mapped));
            std::memcpy(mapped, vertices.data(), sizeof(Render::MeshVertex) * vertices.size());
            vkUnmapMemory(mDevice->handle(), mMeshVertexMemory);

            mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), mMeshIndexMemory, 0,
                sizeof(uint32_t) * indices.size(), 0, &mapped));
            std::memcpy(mapped, indices.data(), sizeof(uint32_t) * indices.size());
            vkUnmapMemory(mDevice->handle(), mMeshIndexMemory);
        }
        catch (...)
        {
            destroyMesh();
            throw;
        }
    }

    void Renderer::destroyMesh()
    {
        if (!mDevice)
            return;

        VkDevice dev = mDevice->handle();
        if (mMeshVertexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(dev, mMeshVertexBuffer, nullptr);
        if (mMeshVertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(dev, mMeshVertexMemory, nullptr);
        if (mMeshIndexBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(dev, mMeshIndexBuffer, nullptr);
        if (mMeshIndexMemory != VK_NULL_HANDLE)
            vkFreeMemory(dev, mMeshIndexMemory, nullptr);

        mMeshVertexBuffer = VK_NULL_HANDLE;
        mMeshVertexMemory = VK_NULL_HANDLE;
        mMeshIndexBuffer = VK_NULL_HANDLE;
        mMeshIndexMemory = VK_NULL_HANDLE;
        mMeshDraws.clear();
    }

    void Renderer::cleanup()
    {
        if (!mDevice)
            return;

        vkDeviceWaitIdle(mDevice->handle());

        VkDevice dev = mDevice->handle();

        destroyMesh();

        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            if (mUniformMapped[i])
            {
                vkUnmapMemory(dev, mUniformMemory[i]);
                mUniformMapped[i] = nullptr;
            }
            if (mUniformBuffers[i] != VK_NULL_HANDLE)
                vkDestroyBuffer(dev, mUniformBuffers[i], nullptr);
            if (mUniformMemory[i] != VK_NULL_HANDLE)
                vkFreeMemory(dev, mUniformMemory[i], nullptr);
        }

        if (mDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(dev, mDescriptorPool, nullptr);
        if (mSceneDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, mSceneDescriptorLayout, nullptr);
        if (mCompositeDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, mCompositeDescriptorLayout, nullptr);
        if (mGBufferSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mGBufferSampler, nullptr);

        if (mGBufferPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferPipeline, nullptr);
        if (mGBufferPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, mGBufferPipelineLayout, nullptr);
        if (mCompositePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mCompositePipeline, nullptr);
        if (mCompositePipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, mCompositePipelineLayout, nullptr);

        if (mGBufferFramebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(dev, mGBufferFramebuffer, nullptr);
        destroyCompositeFramebuffers();

        destroyGBuffer();

        if (mGBufferRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(dev, mGBufferRenderPass, nullptr);
        if (mCompositeRenderPass != VK_NULL_HANDLE)
            vkDestroyRenderPass(dev, mCompositeRenderPass, nullptr);

        mFrameSync.reset();
        mCommandPool.reset();
        mSwapchain.reset();
        mDevice.reset();

        if (mSurface != VK_NULL_HANDLE)
            vkDestroySurfaceKHR(mInstance->handle(), mSurface, nullptr);

        mInstance.reset();
    }
}
