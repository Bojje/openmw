#include "vkrenderer.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>

#include <SDL_vulkan.h>

#include "../render/math.hpp"
#include "../render/terrainmesh.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"
#include "vkinstance.hpp"
#include "vkshader.hpp"
#include "vkswapchain.hpp"
#include "vksync.hpp"

namespace Vk
{
    constexpr std::size_t gbufferPushConstantSize
        = sizeof(Render::Mat4) + sizeof(float) * 12 + sizeof(std::uint32_t) * 2;
    static_assert(gbufferPushConstantSize == 120);

    static void createBufferLocal(Device& device, VkDeviceSize size,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(device.handle(), &bufferInfo, nullptr, &buffer));

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device.handle(), buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(memRequirements.memoryTypeBits, properties);

        VK_CHECK(vkAllocateMemory(device.handle(), &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(device.handle(), buffer, memory, 0));
    }

    static std::string embeddedTextureKey(const Render::TextureData& texture)
    {
        std::string key("\0terrain-alpha:", 15);
        key += std::to_string(texture.width);
        key.push_back('x');
        key += std::to_string(texture.height);
        key.push_back(':');
        key.append(reinterpret_cast<const char*>(texture.pixels.data()), texture.pixels.size());
        return key;
    }

    Renderer::Renderer(SDL_Window* window, bool enableValidation, SurfaceMode surfaceMode,
        uint32_t width, uint32_t height)
        : mWindow(window)
        , mHeadless(surfaceMode == SurfaceMode::Headless)
    {
        if (!mHeadless && mWindow == nullptr)
            throw std::invalid_argument("Window Vulkan surface mode requires an SDL window");
        if (mHeadless && (width == 0 || height == 0))
            throw std::invalid_argument("Headless Vulkan surface requires a non-zero extent");

        mInstance = std::make_unique<Instance>("OpenMW", "OpenMW Engine", enableValidation, mHeadless);
        createSurface();

        mDevice = std::make_unique<Device>(*mInstance, mSurface);

        int drawableWidth = static_cast<int>(width);
        int drawableHeight = static_cast<int>(height);
        if (!mHeadless)
            SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
            throw std::runtime_error("Vulkan surface returned an unavailable drawable size");

        mSwapchain = std::make_unique<Swapchain>(*mDevice, mSurface,
            static_cast<uint32_t>(drawableWidth), static_cast<uint32_t>(drawableHeight), !mHeadless);
        mCommandPool = std::make_unique<CommandPool>(*mDevice, mDevice->indices().graphics.value());
        mFrameSync = std::make_unique<FrameSync>(*mDevice, mSwapchain->imageCount());

        mCommandBuffers = mCommandPool->allocateMultiple(maxFramesInFlight);
        mUploadCommandBuffer = mCommandPool->allocateMultiple(1).front();

        createGBufferRenderPass();
        createCompositeRenderPass();
        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();
        createGBufferSampler();
        Render::TextureData fallbackTexture;
        fallbackTexture.width = 1;
        fallbackTexture.height = 1;
        fallbackTexture.pixels = { 255, 255, 255, 255 };
        const uint32_t fallbackIndex = createTextureResource(fallbackTexture);
        if (fallbackIndex != 0)
            throw std::runtime_error("Vulkan fallback texture was not allocated at index zero");
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

    bool Renderer::validationEnabled() const
    {
        return mInstance != nullptr && mInstance->validationEnabled();
    }

    uint32_t Renderer::validationErrorCount() const
    {
        return mInstance != nullptr ? mInstance->validationErrorCount() : 0;
    }

    void Renderer::createSurface()
    {
        if (mHeadless)
        {
            const auto createHeadlessSurface = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
                vkGetInstanceProcAddr(mInstance->handle(), "vkCreateHeadlessSurfaceEXT"));
            if (createHeadlessSurface == nullptr)
                throw std::runtime_error("Vulkan headless surface entry point is unavailable");

            VkHeadlessSurfaceCreateInfoEXT createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
            VK_CHECK(createHeadlessSurface(mInstance->handle(), &createInfo, nullptr, &mSurface));
            return;
        }

        if (!SDL_Vulkan_CreateSurface(mWindow, mInstance->handle(), &mSurface))
            throw std::runtime_error("Failed to create Vulkan surface via SDL");
    }

    std::pair<uint32_t, uint32_t> Renderer::drawableSize() const
    {
        if (mHeadless)
            return { mSwapchain->extent().width, mSwapchain->extent().height };

        int width = 0;
        int height = 0;
        SDL_Vulkan_GetDrawableSize(mWindow, &width, &height);
        return { width > 0 ? static_cast<uint32_t>(width) : 0,
            height > 0 ? static_cast<uint32_t>(height) : 0 };
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

    uint32_t Renderer::createTextureResource(const Render::TextureData& texture,
        TextureResource::SamplerMode samplerMode)
    {
        if (!texture.valid())
            throw std::invalid_argument("Cannot upload invalid Vulkan texture data");
        if (mTextures.size() >= maxTextures)
            throw std::runtime_error("Vulkan texture table is full");

        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        TextureResource resource;
        resource.samplerMode = samplerMode;
        try
        {
            createBufferLocal(*mDevice, texture.pixels.size(),
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                stagingBuffer, stagingMemory);

            void* mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), stagingMemory, 0, texture.pixels.size(), 0, &mapped));
            std::memcpy(mapped, texture.pixels.data(), texture.pixels.size());
            vkUnmapMemory(mDevice->handle(), stagingMemory);

            createImage(texture.width, texture.height, VK_FORMAT_R8G8B8A8_UNORM,
                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                resource.image, resource.memory);
            resource.view = createImageView(resource.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

            VK_CHECK(vkResetCommandBuffer(mUploadCommandBuffer, 0));
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(mUploadCommandBuffer, &beginInfo));

            transitionImageLayout(mUploadCommandBuffer, resource.image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { texture.width, texture.height, 1 };
            vkCmdCopyBufferToImage(mUploadCommandBuffer, stagingBuffer, resource.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            transitionImageLayout(mUploadCommandBuffer, resource.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
            VK_CHECK(vkEndCommandBuffer(mUploadCommandBuffer));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &mUploadCommandBuffer;
            VK_CHECK(vkQueueSubmit(mDevice->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
            VK_CHECK(vkQueueWaitIdle(mDevice->graphicsQueue()));

            vkDestroyBuffer(mDevice->handle(), stagingBuffer, nullptr);
            vkFreeMemory(mDevice->handle(), stagingMemory, nullptr);

            const uint32_t index = static_cast<uint32_t>(mTextures.size());
            mTextures.push_back(resource);
            return index;
        }
        catch (...)
        {
            if (stagingBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(mDevice->handle(), stagingBuffer, nullptr);
            if (stagingMemory != VK_NULL_HANDLE)
                vkFreeMemory(mDevice->handle(), stagingMemory, nullptr);
            if (resource.view != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice->handle(), resource.view, nullptr);
            if (resource.image != VK_NULL_HANDLE)
                vkDestroyImage(mDevice->handle(), resource.image, nullptr);
            if (resource.memory != VK_NULL_HANDLE)
                vkFreeMemory(mDevice->handle(), resource.memory, nullptr);
            throw;
        }
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
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
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

        createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.specularImage, mGBuffer.specularMemory);
        mGBuffer.specularView
            = createImageView(mGBuffer.specularImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

        createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.emissiveImage, mGBuffer.emissiveMemory);
        mGBuffer.emissiveView
            = createImageView(mGBuffer.emissiveImage, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

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
        destroyAttachment(mGBuffer.specularImage, mGBuffer.specularMemory, mGBuffer.specularView);
        destroyAttachment(mGBuffer.emissiveImage, mGBuffer.emissiveMemory, mGBuffer.emissiveView);
        destroyAttachment(mGBuffer.materialImage, mGBuffer.materialMemory, mGBuffer.materialView);
        destroyAttachment(mGBuffer.depthImage, mGBuffer.depthMemory, mGBuffer.depthView);
    }

    // Render passes

    void Renderer::createGBufferRenderPass()
    {
        VkFormat depthFormat = findDepthFormat();

        std::array<VkAttachmentDescription, 6> attachments = {};

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

        // Specular color
        attachments[3].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[3].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[3].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[3].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Emissive color
        attachments[4].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[4].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[4].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[4].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[4].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[4].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[4].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[4].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        // Depth
        attachments[5].format = depthFormat;
        attachments[5].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[5].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[5].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[5].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[5].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[5].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[5].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        std::array<VkAttachmentReference, 5> colorRefs = {};
        colorRefs[0] = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[1] = { 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[2] = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[3] = { 3, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        colorRefs[4] = { 4, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

        VkAttachmentReference depthRef = { 5, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };

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
        std::array<VkImageView, 6> attachments = {
            mGBuffer.albedoView,
            mGBuffer.normalView,
            mGBuffer.materialView,
            mGBuffer.specularView,
            mGBuffer.emissiveView,
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
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mGBufferSampler));

        // Scene textures use tiled UVs for terrain and must repeat. The G-buffer
        // sampler above remains clamped because its coordinates are screen-space.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mSceneSampler));

        // Blendmaps are finite masks, not tiled scene textures. Clamp their
        // edge samples so filtering cannot pull opacity from the opposite
        // side of the image.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mAlphaSampler));

        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mRepeatUSampler));
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mRepeatVSampler));
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

    void Renderer::writeSceneTextureDescriptor(
        uint32_t frameIndex, uint32_t binding, uint32_t textureIndex, VkImageView view)
    {
        if (frameIndex >= maxFramesInFlight)
            throw std::out_of_range("Vulkan frame index is out of range");
        if (textureIndex >= maxTextures)
            throw std::out_of_range("Vulkan texture descriptor index is out of range");

        VkDescriptorImageInfo imageInfo = {};
        if (binding == 2)
            imageInfo.sampler = mAlphaSampler;
        else
        {
            switch (mTextures.at(textureIndex).samplerMode)
            {
                case TextureResource::SamplerMode::Repeat:
                    imageInfo.sampler = mSceneSampler;
                    break;
                case TextureResource::SamplerMode::Clamp:
                    imageInfo.sampler = mAlphaSampler;
                    break;
                case TextureResource::SamplerMode::RepeatU:
                    imageInfo.sampler = mRepeatUSampler;
                    break;
                case TextureResource::SamplerMode::RepeatV:
                    imageInfo.sampler = mRepeatVSampler;
                    break;
            }
        }
        imageInfo.imageView = view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = mSceneDescriptorSets[frameIndex];
        write.dstBinding = binding;
        write.dstArrayElement = textureIndex;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
    }

    void Renderer::syncSceneTextureDescriptors(uint32_t frameIndex)
    {
        for (uint32_t textureIndex = 0; textureIndex < mTextures.size(); ++textureIndex)
        {
            const VkImageView view = mTextures[textureIndex].view;
            writeSceneTextureDescriptor(frameIndex, 1, textureIndex, view);
            writeSceneTextureDescriptor(frameIndex, 2, textureIndex, view);
            writeSceneTextureDescriptor(frameIndex, 3, textureIndex, view);
            writeSceneTextureDescriptor(frameIndex, 4, textureIndex, view);
            writeSceneTextureDescriptor(frameIndex, 5, textureIndex, view);
        }
    }

    // Descriptor set layouts

    void Renderer::createDescriptorSetLayouts()
    {
        // Scene layout (set 0 for G-buffer pass): camera UBO and indexed
        // albedo, alpha, normal, emissive, and specular textures.
        {
            std::array<VkDescriptorSetLayoutBinding, 6> bindings = {};
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = maxTextures;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = maxTextures;
            bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[3].binding = 3;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[3].descriptorCount = maxTextures;
            bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[4].binding = 4;
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[4].descriptorCount = maxTextures;
            bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            bindings[5].binding = 5;
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[5].descriptorCount = maxTextures;
            bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mSceneDescriptorLayout));
        }

        // Composite layout: G-buffer textures, scene UBO, and material data
        {
            std::array<VkDescriptorSetLayoutBinding, 7> bindings = {};

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

            // Specular color
            bindings[5].binding = 6;
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[5].descriptorCount = 1;
            bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Emissive color
            bindings[6].binding = 7;
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[6].descriptorCount = 1;
            bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

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
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, maxTextures * maxFramesInFlight * 5 + maxFramesInFlight * 6 },
        };

        const uint32_t maxSets = maxFramesInFlight * 2;

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
            createBufferLocal(*mDevice, bufferSize,
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

            for (uint32_t frameIndex = 0; frameIndex < maxFramesInFlight; ++frameIndex)
                syncSceneTextureDescriptors(frameIndex);
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
            writeCompositeDescriptor(6, mGBuffer.specularView);
            writeCompositeDescriptor(7, mGBuffer.emissiveView);

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
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = gbufferPushConstantSize;

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
            std::clog << "Vulkan SPIR-V shaders not found in " << shaderDir
                      << ", rendering disabled until shaders are compiled\n";
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
            bindingDesc[0].stride = sizeof(Render::MeshVertex);
            bindingDesc[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::array<VkVertexInputAttributeDescription, 8> attrDesc = {};
            attrDesc[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 };
            attrDesc[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3 };
            attrDesc[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6 };
            attrDesc[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 10 };
            attrDesc[4] = { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 14 };
            attrDesc[5] = { 5, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 8 };
            attrDesc[6] = { 6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 18 };
            attrDesc[7] = { 7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, sizeof(float) * 22 };

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

            std::array<VkPipelineColorBlendAttachmentState, 5> blendAttachments = {};
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

            rasterizer.cullMode = VK_CULL_MODE_NONE;
            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferDoubleSidedPipeline));
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;

            // Alpha-blended materials use a separate pipeline so opaque draws
            // retain depth writes. The G-buffer blend is intentionally simple;
            // final ordering and material composition remain migration work.
            blendAttachments[0].blendEnable = VK_TRUE;
            blendAttachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachments[0].colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachments[0].alphaBlendOp = VK_BLEND_OP_ADD;
            blendAttachments[3].blendEnable = VK_TRUE;
            blendAttachments[3].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachments[3].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachments[3].colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachments[3].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachments[3].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachments[3].alphaBlendOp = VK_BLEND_OP_ADD;
            blendAttachments[4] = blendAttachments[0];
            depthStencil.depthWriteEnable = VK_FALSE;

            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferAlphaPipeline));

            rasterizer.cullMode = VK_CULL_MODE_NONE;
            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferAlphaDoubleSidedPipeline));
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;

            // Terrain layer zero writes the depth and replaces the G-buffer
            // albedo with its blendmap-weighted color. Later layers use the
            // same depth and add their weighted color in submission order.
            blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
            blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blendAttachments[3].blendEnable = VK_FALSE;
            blendAttachments[4].blendEnable = VK_FALSE;
            blendAttachments[1].blendEnable = VK_FALSE;
            blendAttachments[2].blendEnable = VK_FALSE;
            depthStencil.depthWriteEnable = VK_TRUE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferTerrainFirstPipeline));

            blendAttachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachments[3].blendEnable = VK_TRUE;
            blendAttachments[3].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blendAttachments[3].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            depthStencil.depthWriteEnable = VK_FALSE;
            depthStencil.depthCompareOp = VK_COMPARE_OP_EQUAL;

            VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &mGBufferTerrainLayerPipeline));
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

        std::clog << "Vulkan pipelines created successfully\n";
        return true;
    }

    // Frame lifecycle

    bool Renderer::beginFrame()
    {
        mFrameSync->waitForFrame(mCurrentFrame);
        syncSceneTextureDescriptors(mCurrentFrame);

        auto [drawableWidth, drawableHeight] = drawableSize();
        if (drawableWidth <= 0 || drawableHeight <= 0)
            return false;

        if (mUploadedMeshRevisions[mCurrentFrame] != mMeshRevision)
            uploadMesh(mCurrentFrame);

        VkResult result;
        for (;;)
        {
            result = vkAcquireNextImageKHR(mDevice->handle(), mSwapchain->handle(),
                UINT64_MAX, mFrameSync->imageAvailable(mCurrentFrame), VK_NULL_HANDLE, &mCurrentImageIndex);

            if (result == VK_ERROR_OUT_OF_DATE_KHR)
            {
                const auto [newWidth, newHeight] = drawableSize();
                drawableWidth = newWidth;
                drawableHeight = newHeight;
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

        if (mHasSceneData)
            std::memcpy(mUniformMapped[mCurrentFrame], &mSceneData, sizeof(Render::SceneData));

        return true;
    }

    bool Renderer::endFrame()
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
            mHasSubmittedFrame = false;
            const auto [w, h] = drawableSize();
            resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
            mCurrentFrame = (mCurrentFrame + 1) % maxFramesInFlight;
            return false;
        }
        else
        {
            VK_CHECK(result);
            mLastSubmittedFrame = mCurrentFrame;
            mLastSubmittedImage = mCurrentImageIndex;
            mHasSubmittedFrame = true;
        }
        mCurrentFrame = (mCurrentFrame + 1) % maxFramesInFlight;
        return true;
    }

    bool Renderer::render()
    {
        if (!beginFrame())
            return false;

        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        VkExtent2D extent = mSwapchain->extent();

        // 1. G-buffer pass
        {
            std::array<VkClearValue, 5> clearValues = {};
            clearValues[0].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[1].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[2].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clearValues[3].color = {{ 1.0f, 1.0f, 1.0f, 1.0f }};
            clearValues[4].depthStencil = { 1.0f, 0 };

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

                const MeshBuffers& meshBuffers = mMeshBuffers[mCurrentFrame];
                if (meshBuffers.vertex != VK_NULL_HANDLE && meshBuffers.index != VK_NULL_HANDLE)
                {
                    struct PushData
                    {
                        Render::Mat4 model;
                        std::array<float, 12> normalMatrix;
                        uint32_t materialFlags;
                        uint32_t textureIndices;
                    };
                    static_assert(sizeof(PushData) == gbufferPushConstantSize);

                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &meshBuffers.vertex, &offset);
                    vkCmdBindIndexBuffer(cmd, meshBuffers.index, 0, VK_INDEX_TYPE_UINT32);
                    VkPipeline boundPipeline = VK_NULL_HANDLE;
                    const Render::Vec3 cameraPosition = { mSceneData.viewInverse.data[12],
                        mSceneData.viewInverse.data[13], mSceneData.viewInverse.data[14] };
                    const std::vector<std::size_t> drawOrder = Render::orderMeshDraws(mMeshDraws, cameraPosition);
                    for (const std::size_t drawIndex : drawOrder)
                    {
                        const Render::MeshDraw& draw = mMeshDraws[drawIndex];
                        const VkPipeline pipeline = draw.material.terrainBlend
                            ? (draw.material.terrainFirstLayer ? mGBufferTerrainFirstPipeline
                                                               : mGBufferTerrainLayerPipeline)
                            : (draw.material.alphaBlend
                                    ? (draw.material.doubleSided ? mGBufferAlphaDoubleSidedPipeline
                                                                 : mGBufferAlphaPipeline)
                                    : (draw.material.doubleSided ? mGBufferDoubleSidedPipeline : mGBufferPipeline));
                        if (pipeline != boundPipeline)
                        {
                            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                            boundPipeline = pipeline;
                        }
                        uint32_t materialFlags = draw.material.alphaTest
                            ? 1u | (static_cast<uint32_t>(draw.material.alphaTestThreshold) << 8u)
                            : 0u;
                        if (draw.material.terrainBlend)
                            materialFlags |= 2u;
                        if (draw.material.normalMap || draw.material.terrainNormalMap)
                            materialFlags |= 4u;
                        if (draw.material.terrainParallax)
                            materialFlags |= 8u;
                        if (draw.material.waterSurface)
                            materialFlags |= 16u;
                        if (draw.material.ambientOverride)
                            materialFlags |= 32u;
                        if (draw.material.emissiveOverride)
                            materialFlags |= 64u;
                        if (draw.material.emissiveAnimated)
                            materialFlags |= 128u;
                        const PushData pushData = {
                            draw.transform,
                            { draw.normalMatrix.data[0], draw.normalMatrix.data[1], draw.normalMatrix.data[2], 0.f,
                                draw.normalMatrix.data[4], draw.normalMatrix.data[5], draw.normalMatrix.data[6], 0.f,
                                draw.normalMatrix.data[8], draw.normalMatrix.data[9], draw.normalMatrix.data[10], 0.f },
                            materialFlags,
                            mMeshTextureIndices[drawIndex] | (mMeshAlphaTextureIndices[drawIndex] << 6u)
                                | (mMeshNormalTextureIndices[drawIndex] << 12u)
                                | (mMeshEmissiveTextureIndices[drawIndex] << 18u)
                                | (mMeshSpecularTextureIndices[drawIndex] << 24u),
                        };
                        vkCmdPushConstants(cmd, mGBufferPipelineLayout,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
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

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }

            vkCmdEndRenderPass(cmd);
        }

        return endFrame();
    }

    std::optional<Render::TextureData> Renderer::captureFrame()
    {
        if (!mHasSubmittedFrame || mHeadless)
            return std::nullopt;

        const VkFormat format = mSwapchain->format();
        const bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
        const bool rgba = format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB;
        if (!bgra && !rgba)
            throw std::runtime_error("Vulkan frame capture requires an RGBA8 or BGRA8 swapchain");

        mFrameSync->waitForFrame(mLastSubmittedFrame);
        VK_CHECK(vkQueueWaitIdle(mDevice->presentQueue()));

        const VkExtent2D extent = mSwapchain->extent();
        const VkDeviceSize byteSize = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
        VkBuffer stagingBuffer = VK_NULL_HANDLE;
        VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
        createBufferLocal(*mDevice, byteSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer, stagingMemory);

        try
        {
            VK_CHECK(vkResetCommandBuffer(mUploadCommandBuffer, 0));
            VkCommandBufferBeginInfo beginInfo = {};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(mUploadCommandBuffer, &beginInfo));

            const VkImage image = mSwapchain->image(mLastSubmittedImage);
            transitionImageLayout(mUploadCommandBuffer, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { extent.width, extent.height, 1 };
            vkCmdCopyImageToBuffer(mUploadCommandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                stagingBuffer, 1, &region);

            transitionImageLayout(mUploadCommandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
            VK_CHECK(vkEndCommandBuffer(mUploadCommandBuffer));

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &mUploadCommandBuffer;
            VK_CHECK(vkQueueSubmit(mDevice->graphicsQueue(), 1, &submitInfo, VK_NULL_HANDLE));
            VK_CHECK(vkQueueWaitIdle(mDevice->graphicsQueue()));

            void* mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), stagingMemory, 0, byteSize, 0, &mapped));
            const auto* source = static_cast<const std::uint8_t*>(mapped);
            Render::TextureData result;
            result.width = extent.width;
            result.height = extent.height;
            result.pixels.resize(static_cast<std::size_t>(byteSize));
            for (std::size_t pixel = 0; pixel < result.pixels.size(); pixel += 4)
            {
                result.pixels[pixel + 0] = source[pixel + (bgra ? 2 : 0)];
                result.pixels[pixel + 1] = source[pixel + 1];
                result.pixels[pixel + 2] = source[pixel + (bgra ? 0 : 2)];
                result.pixels[pixel + 3] = source[pixel + 3];
            }
            vkUnmapMemory(mDevice->handle(), stagingMemory);

            vkDestroyBuffer(mDevice->handle(), stagingBuffer, nullptr);
            vkFreeMemory(mDevice->handle(), stagingMemory, nullptr);
            return result;
        }
        catch (...)
        {
            vkDestroyBuffer(mDevice->handle(), stagingBuffer, nullptr);
            vkFreeMemory(mDevice->handle(), stagingMemory, nullptr);
            throw;
        }
    }

    void Renderer::resize()
    {
        const auto [width, height] = drawableSize();
        if (width > 0 && height > 0)
            resize(width, height);
    }

    void Renderer::resize(uint32_t width, uint32_t height)
    {
        if (width == 0 || height == 0)
            return;

        mHasSubmittedFrame = false;

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
        writeCompositeDescriptor(6, mGBuffer.specularView);
        writeCompositeDescriptor(7, mGBuffer.emissiveView);
    }

    void Renderer::setScene(const Render::SceneSubmission& submission)
    {
        if (const std::string error = submission.validationError(); !error.empty())
            throw std::invalid_argument("Vulkan scene submission: " + error);

        mSceneData = submission.scene;
        mHasSceneData = true;
        mDynamicMeshCount = submission.dynamicMeshes.size();
        std::vector<Render::MeshInstance> meshes = submission.meshes;
        for (const Render::EffectMeshSubmission& effect : submission.effects)
            meshes.insert(meshes.end(), effect.meshes.begin(), effect.meshes.end());
        for (const Render::TerrainTile& tile : submission.terrainTiles)
        {
            std::vector<Render::MeshInstance> terrain = Render::makeTerrainMeshes(tile);
            meshes.insert(meshes.end(), std::make_move_iterator(terrain.begin()),
                std::make_move_iterator(terrain.end()));
        }
        std::vector<Render::MeshInstance> rasterDynamic = Render::collectRasterDynamicMeshes(submission);
        meshes.insert(meshes.end(), std::make_move_iterator(rasterDynamic.begin()),
            std::make_move_iterator(rasterDynamic.end()));
        setMeshes(meshes, submission.textureResolver);
    }

    void Renderer::setMeshes(const std::vector<Render::MeshInstance>& meshes, Render::TextureResolver textureResolver)
    {
        Render::MeshBatch batch = Render::batchMeshes(meshes);
        std::vector<uint32_t> textureIndices;
        std::vector<uint32_t> alphaTextureIndices;
        std::vector<uint32_t> normalTextureIndices;
        std::vector<uint32_t> emissiveTextureIndices;
        std::vector<uint32_t> specularTextureIndices;
        textureIndices.reserve(batch.draws.size());
        alphaTextureIndices.reserve(batch.draws.size());
        normalTextureIndices.reserve(batch.draws.size());
        emissiveTextureIndices.reserve(batch.draws.size());
        specularTextureIndices.reserve(batch.draws.size());

        const auto resolveTexture = [&](std::string_view path, bool wrapU, bool wrapV) {
            if (path.empty() || !textureResolver)
                return uint32_t(0);

            const TextureResource::SamplerMode samplerMode = wrapU
                ? (wrapV ? TextureResource::SamplerMode::Repeat : TextureResource::SamplerMode::RepeatU)
                : (wrapV ? TextureResource::SamplerMode::RepeatV : TextureResource::SamplerMode::Clamp);
            std::string key(path);
            key.push_back('\0');
            key.push_back(static_cast<char>(samplerMode));
            const auto existing = mTextureIndices.find(key);
            if (existing != mTextureIndices.end())
                return existing->second;

            const std::shared_ptr<const Render::TextureData> texture = textureResolver(path);
            const uint32_t textureIndex = texture && texture->valid()
                ? createTextureResource(*texture, samplerMode)
                : 0;
            mTextureIndices.emplace(key, textureIndex);
            return textureIndex;
        };

        for (Render::MeshDraw& draw : batch.draws)
        {
            textureIndices.push_back(
                resolveTexture(draw.material.albedoTexture, draw.material.albedoWrapU, draw.material.albedoWrapV));
            const bool wantsNormalMap = draw.material.normalMap || draw.material.terrainNormalMap;
            const uint32_t normalTextureIndex = wantsNormalMap
                ? resolveTexture(draw.material.normalTexture, draw.material.normalWrapU, draw.material.normalWrapV)
                : 0;
            draw.material.normalMap = draw.material.normalMap && normalTextureIndex != 0;
            draw.material.terrainNormalMap = draw.material.terrainNormalMap && normalTextureIndex != 0;
            draw.material.terrainParallax = draw.material.terrainParallax && draw.material.terrainNormalMap;
            normalTextureIndices.push_back(normalTextureIndex);

            emissiveTextureIndices.push_back(
                resolveTexture(draw.material.emissiveTexture, draw.material.emissiveWrapU, draw.material.emissiveWrapV));
            specularTextureIndices.push_back(
                resolveTexture(draw.material.specularTexture, draw.material.specularWrapU,
                    draw.material.specularWrapV));
        }

        for (const Render::MeshDraw& draw : batch.draws)
        {
            if (!draw.material.alphaTexture || !draw.material.alphaTexture->valid())
            {
                alphaTextureIndices.push_back(0);
                continue;
            }

            const std::string key = embeddedTextureKey(*draw.material.alphaTexture);
            const auto alphaExisting = mTextureIndices.find(key);
            if (alphaExisting != mTextureIndices.end())
            {
                alphaTextureIndices.push_back(alphaExisting->second);
                continue;
            }

            const uint32_t alphaTextureIndex
                = createTextureResource(*draw.material.alphaTexture, TextureResource::SamplerMode::Clamp);
            mTextureIndices.emplace(key, alphaTextureIndex);
            alphaTextureIndices.push_back(alphaTextureIndex);
        }

        mMeshDraws = std::move(batch.draws);
        mMeshVertices = std::move(batch.vertices);
        mMeshIndices = std::move(batch.indices);
        mMeshTextureIndices = std::move(textureIndices);
        mMeshAlphaTextureIndices = std::move(alphaTextureIndices);
        mMeshNormalTextureIndices = std::move(normalTextureIndices);
        mMeshEmissiveTextureIndices = std::move(emissiveTextureIndices);
        mMeshSpecularTextureIndices = std::move(specularTextureIndices);
        ++mMeshRevision;
        if (mMeshRevision == 0)
            ++mMeshRevision;
    }

    void Renderer::uploadMesh(uint32_t frameIndex)
    {
        destroyMesh(frameIndex);

        MeshBuffers& buffers = mMeshBuffers.at(frameIndex);
        if (mMeshVertices.empty() || mMeshIndices.empty())
        {
            mUploadedMeshRevisions[frameIndex] = mMeshRevision;
            return;
        }

        try
        {
            createBufferLocal(*mDevice,
                sizeof(Render::MeshVertex) * mMeshVertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                buffers.vertex, buffers.vertexMemory);
            createBufferLocal(*mDevice,
                sizeof(uint32_t) * mMeshIndices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                buffers.index, buffers.indexMemory);

            void* mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), buffers.vertexMemory, 0,
                sizeof(Render::MeshVertex) * mMeshVertices.size(), 0, &mapped));
            std::memcpy(mapped, mMeshVertices.data(), sizeof(Render::MeshVertex) * mMeshVertices.size());
            vkUnmapMemory(mDevice->handle(), buffers.vertexMemory);

            mapped = nullptr;
            VK_CHECK(vkMapMemory(mDevice->handle(), buffers.indexMemory, 0,
                sizeof(uint32_t) * mMeshIndices.size(), 0, &mapped));
            std::memcpy(mapped, mMeshIndices.data(), sizeof(uint32_t) * mMeshIndices.size());
            vkUnmapMemory(mDevice->handle(), buffers.indexMemory);
            mUploadedMeshRevisions[frameIndex] = mMeshRevision;
        }
        catch (...)
        {
            destroyMesh(frameIndex);
            throw;
        }
    }

    void Renderer::destroyMesh()
    {
        if (!mDevice)
            return;

        for (uint32_t frameIndex = 0; frameIndex < maxFramesInFlight; ++frameIndex)
            destroyMesh(frameIndex);
        mUploadedMeshRevisions = {};
        mMeshDraws.clear();
        mMeshVertices.clear();
        mMeshIndices.clear();
        mMeshTextureIndices.clear();
        mMeshAlphaTextureIndices.clear();
        mMeshNormalTextureIndices.clear();
        mMeshSpecularTextureIndices.clear();
        mDynamicMeshCount = 0;
    }

    void Renderer::destroyMesh(uint32_t frameIndex)
    {
        if (!mDevice)
            return;

        VkDevice dev = mDevice->handle();
        MeshBuffers& buffers = mMeshBuffers.at(frameIndex);
        if (buffers.vertex != VK_NULL_HANDLE)
            vkDestroyBuffer(dev, buffers.vertex, nullptr);
        if (buffers.vertexMemory != VK_NULL_HANDLE)
            vkFreeMemory(dev, buffers.vertexMemory, nullptr);
        if (buffers.index != VK_NULL_HANDLE)
            vkDestroyBuffer(dev, buffers.index, nullptr);
        if (buffers.indexMemory != VK_NULL_HANDLE)
            vkFreeMemory(dev, buffers.indexMemory, nullptr);
        buffers = {};
        mUploadedMeshRevisions[frameIndex] = 0;
    }

    void Renderer::destroyTextures()
    {
        if (!mDevice)
            return;

        VkDevice dev = mDevice->handle();
        for (TextureResource& texture : mTextures)
        {
            if (texture.view != VK_NULL_HANDLE)
                vkDestroyImageView(dev, texture.view, nullptr);
            if (texture.image != VK_NULL_HANDLE)
                vkDestroyImage(dev, texture.image, nullptr);
            if (texture.memory != VK_NULL_HANDLE)
                vkFreeMemory(dev, texture.memory, nullptr);
        }
        mTextures.clear();
        mTextureIndices.clear();
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
        destroyTextures();
        if (mGBufferSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mGBufferSampler, nullptr);
        if (mSceneSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mSceneSampler, nullptr);
        if (mAlphaSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mAlphaSampler, nullptr);
        if (mRepeatUSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mRepeatUSampler, nullptr);
        if (mRepeatVSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mRepeatVSampler, nullptr);

        if (mGBufferPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferPipeline, nullptr);
        if (mGBufferDoubleSidedPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferDoubleSidedPipeline, nullptr);
        if (mGBufferAlphaPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferAlphaPipeline, nullptr);
        if (mGBufferAlphaDoubleSidedPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferAlphaDoubleSidedPipeline, nullptr);
        if (mGBufferTerrainFirstPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferTerrainFirstPipeline, nullptr);
        if (mGBufferTerrainLayerPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferTerrainLayerPipeline, nullptr);
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
