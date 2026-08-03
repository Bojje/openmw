#include "vkrenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <vk_mem_alloc.h>

#include <components/debug/debuglog.hpp>

#include "vkbuffer.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"
#include "vkinstance.hpp"
#include "vkplatform.hpp"
#include "vkraytracing.hpp"
#include "vkshader.hpp"
#include "vkswapchain.hpp"
#include "vksync.hpp"
#include "vktexture.hpp"

namespace Vk
{
    static void createBufferLocal(const Device& device, VkDeviceSize size,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        VkBuffer& buffer, VmaAllocation& allocation)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        // Both callers keep their buffer mapped for the lifetime of the renderer and only ever write
        // through it, never read back.
        allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        // The caller's property flags are a floor, not a replacement for VMA's own choice: AUTO would
        // otherwise be free to pick device-local memory that cannot be mapped.
        allocInfo.requiredFlags = properties;

        VK_CHECK(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr));
    }

    Renderer::Renderer(SDL_Window* window, bool enableValidation)
        : mWindow(window)
    {
        mInstance = std::make_unique<Instance>("OpenMW", "OpenMW Engine", enableValidation);
        createSurface();

        mDevice = std::make_unique<Device>(*mInstance, mSurface);

        int w, h;
        getDrawableSize(window, w, h);

        mSwapchain = std::make_unique<Swapchain>(*mDevice, mSurface,
            static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        mCommandPool = std::make_unique<CommandPool>(*mDevice, mDevice->indices().graphics.value());
        mFrameSync = std::make_unique<FrameSync>(
            *mDevice, static_cast<uint32_t>(mSwapchain->imageViews().size()));

        mCommandBuffers = mCommandPool->allocateMultiple(maxFramesInFlight);

        createGBufferRenderPass();
        createCompositeRenderPass();
        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();
        createGBufferSampler();
        createTextureSampler();
        // Must exist before the descriptor sets are written: every slot of the sampler array is
        // initialised to this texture's view.
        createFallbackTexture();
        createDescriptorSetLayouts();
        createDescriptorPool();
        createUniformBuffers();
        // Before createDescriptorSets: the composite set binds these at binding 6.
        createLightBuffers();
        // Likewise, at binding 2 of the scene set.
        createSkinBuffers();
        // And binding 3.
        createParticleBuffers();
        createDescriptorSets();
        createGBufferPipeline();
        createCompositePipeline();

        if (mDevice->rayTracingSupported())
        {
            loadRayTracingFunctions(mDevice->handle());
            createRtOutput();
            // Before createRtDescriptorSets, which binds their views.
            createDenoiseTargets();
            createRtDescriptorSets();
            mRayTracingEnabled = true;

            writeCompositeDescriptor(3, mRtOutput.view);
            writeCompositeDescriptor(7, mRtIndirect.view);
            writeCompositeHistoryDescriptors();
        }
    }

    Renderer::~Renderer()
    {
        cleanup();
    }

    void Renderer::createSurface()
    {
        mSurface = createPlatformSurface(mInstance->handle(), mWindow);
    }

    VkExtent2D Renderer::swapchainExtent() const
    {
        return mSwapchain->extent();
    }

    void Renderer::createImage(uint32_t width, uint32_t height, VkFormat format,
        VkImageUsageFlags usage, VkImage& image, VmaAllocation& allocation)
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

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        // Every caller is a full-screen render target, which does want a dedicated allocation -- but
        // DEDICATED_MEMORY_BIT is deliberately not set. VMA already promotes allocations that are large
        // relative to the heap's block size, so the render targets still get their own VkDeviceMemory as
        // they did before; forcing it would only rule out the cases where VMA knows better.
        VK_CHECK(vmaCreateImage(mDevice->allocator(), &imageInfo, &allocInfo, &image, &allocation, nullptr));
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
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_GENERAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
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
        else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL)
        {
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            dstStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
            && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            // Sampled by the composite fragment shader and, once it is in SHADER_READ_ONLY, potentially
            // by the hit shaders too.
            dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_GENERAL)
        {
            // The denoise history images, on creation: cleared, then handed to raygen, which is the
            // only thing that ever touches them and does so as a storage image in both directions.
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_GENERAL && newLayout == VK_IMAGE_LAYOUT_GENERAL)
        {
            // Not a layout change at all -- a cross-frame memory dependency on the denoise history.
            // Frame N's raygen reads what frame N-1's raygen wrote, and nothing else establishes that.
            // Submission order on one queue gives execution order, not availability and visibility;
            // beginFrame's fence retires frame N-2, not N-1; and the swapchain semaphores chain
            // nothing about these images. So the barrier is load-bearing despite looking like a no-op.
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            srcStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
            dstStage = VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR && newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        {
            // Taking a screenshot: the composite pass has just finished writing the swapchain image
            // and the copy out of it reads it. Legal only between acquire and present -- see
            // Renderer::requestScreenshot.
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            srcStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)
        {
            // And handing it back, because endFrame presents it immediately afterwards.
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = 0;
            srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        }
        else
        {
            // Deliberately conservative: an unhandled pair is a bug, but stalling everything is at
            // least correct. Add an explicit branch above rather than leaning on this.
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

        // _SRGB, not UNORM. gbuffer.frag writes *linear* albedo here, and 8 bits of linear is not
        // enough in the shadows: sRGB codes 0-34 all collapse into linear codes 0-4, so the darkest
        // eighth of every texture posterises and anything below sRGB 5 rounds to black. An _SRGB
        // attachment makes the hardware encode on store and decode on sample, which costs nothing and
        // restores the precision. Normal and material stay UNORM -- they are not colour.
        createImage(extent.width, extent.height, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            mGBuffer.albedoImage, mGBuffer.albedoMemory);
        mGBuffer.albedoView = createImageView(mGBuffer.albedoImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT);

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
        VmaAllocator allocator = mDevice->allocator();

        // vmaDestroyImage destroys the VkImage as well as freeing the allocation, so there is no
        // separate vkDestroyImage here.
        auto destroyAttachment = [dev, allocator](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
            if (view != VK_NULL_HANDLE) { vkDestroyImageView(dev, view, nullptr); view = VK_NULL_HANDLE; }
            if (img != VK_NULL_HANDLE)
            {
                vmaDestroyImage(allocator, img, alloc);
                img = VK_NULL_HANDLE;
                alloc = VK_NULL_HANDLE;
            }
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

        // Albedo. Must match createGBuffer's _SRGB choice -- see the comment there.
        attachments[0].format = VK_FORMAT_R8G8B8A8_SRGB;
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

        // There is one G-buffer shared by both frames in flight, and both the RT pass and the composite
        // pass sample it. Two dependencies are needed, and the second one is the easy one to omit:
        // without it Vulkan supplies an implicit EXTERNAL dependency whose dstStageMask is
        // BOTTOM_OF_PIPE and dstAccessMask 0, which makes the attachment writes available but never
        // visible to the shader reads that follow. It happens to work on drivers that flush
        // conservatively at render pass end; it is not guaranteed.
        std::array<VkSubpassDependency, 2> dependencies = {};

        // Write-after-read: frame N overwrites the attachments that frame N-1's composite fragment
        // shader and raygen shader may still be reading, so those stages must be named as the source.
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
            | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].dstAccessMask
            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        // Read-after-write: make this frame's attachment writes visible to the RT and composite passes.
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask
            = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].srcAccessMask
            = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstStageMask
            = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;
        dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
        renderPassInfo.pDependencies = dependencies.data();

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

    void Renderer::createTextureSampler()
    {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        // Trilinear. NEAREST between levels leaves a visible seam where the LOD flips, which on
        // Morrowind's terrain is a moving band across the ground as the camera advances.
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        // Not a fixed number: one sampler serves every texture, and Vulkan already clamps the computed
        // LOD to the view's own level count. An unclamped sampler therefore cannot over-run a small
        // texture, whereas any fixed maxLod would clamp a large one.
        samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
        // Morrowind's UVs routinely run outside [0, 1] and rely on wrapping.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        // Trilinear alone makes ground at grazing angles blurrier than it is today, which is exactly
        // Morrowind's terrain -- the thing mip mapping was added to fix. Anisotropy is what buys that
        // back. Gated on the feature actually being enabled: vkdevice.cpp requests samplerAnisotropy
        // conditionally, so setting this unguarded is a validation error on a device that lacks it.
        VkPhysicalDeviceFeatures supported = {};
        vkGetPhysicalDeviceFeatures(mDevice->physical(), &supported);
        if (supported.samplerAnisotropy)
        {
            VkPhysicalDeviceProperties props = {};
            vkGetPhysicalDeviceProperties(mDevice->physical(), &props);
            samplerInfo.anisotropyEnable = VK_TRUE;
            // 8x is the usual quality knee; 16x costs measurably more for little visible gain.
            samplerInfo.maxAnisotropy = std::min(8.0f, props.limits.maxSamplerAnisotropy);
        }

        VK_CHECK(vkCreateSampler(mDevice->handle(), &samplerInfo, nullptr, &mTextureSampler));
    }

    void Renderer::createFallbackTexture()
    {
        // Opaque white so that an untextured mesh renders as its plain vertex colour.
        const std::array<uint8_t, 4> white = { 255, 255, 255, 255 };
        mFallbackTexture = std::make_unique<Texture>(Texture::create(*mDevice, *mCommandPool, 1, 1,
            VK_FORMAT_R8G8B8A8_UNORM, white.data(), white.size()));

        mTextureViews.assign(maxSceneTextures, mFallbackTexture->view());
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

    void Renderer::writeCompositeHistoryDescriptors()
    {
        // Per frame rather than the same view for every set, unlike every other composite binding:
        // the history ping-pongs, and composite has to read whichever half raygen just wrote, which
        // is the one indexed by the frame being recorded.
        //
        // Bound in GENERAL rather than SHADER_READ_ONLY. Sampling from GENERAL is legal, and the
        // alternative -- transitioning the history to SHADER_READ_ONLY for the composite pass and
        // back to GENERAL for the next frame's raygen -- would add two layout transitions per frame
        // to an image whose whole point is that it never changes layout.
        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            VkDescriptorImageInfo imageInfo = {};
            imageInfo.sampler = mGBufferSampler;
            imageInfo.imageView = mDenoiseHistory[i].view;
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = mCompositeDescriptorSets[i];
            write.dstBinding = 8;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
        }
    }

    // Descriptor set layouts

    void Renderer::createDescriptorSetLayouts()
    {
        // Scene layout (set 0 for G-buffer pass): camera UBO + the scene texture array
        {
            std::array<VkDescriptorSetLayoutBinding, 4> bindings = {};

            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

            // Fixed-size array of scene textures. No descriptor indexing: the index comes from a push
            // constant and is uniform across the draw call.
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[1].descriptorCount = maxSceneTextures;
            bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Bone palettes for the skinned pipeline. Declared on the shared scene layout rather than
            // on a layout of its own so both G-buffer pipelines can use one descriptor set, which is
            // what lets the draw loop switch between them without rebinding anything.
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

            // Particle quads. On the scene set rather than a set of its own so the particle pipeline
            // can reach the camera and the sampler array through the same binding, which is what lets
            // it draw inside the composite pass without any descriptor plumbing of its own.
            bindings[3].binding = 3;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mSceneDescriptorLayout));
        }

        // Composite layout: G-buffer textures + RT output + scene UBO
        {
            std::array<VkDescriptorSetLayoutBinding, 9> bindings = {};

            // Denoise history, for the per-pixel history length. composite runs a spatial fallback
            // filter over pixels the temporal accumulator has nothing for, and this is how it knows
            // which those are.
            bindings[8].binding = 8;
            bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[8].descriptorCount = 1;
            bindings[8].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Ray traced ambient occlusion. This is what replaces the sShadowFloor placeholder: the
            // floor existed only because a fully shadowed surface had no occlusion term of any kind
            // and crushed to black.
            bindings[7].binding = 7;
            bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[7].descriptorCount = 1;
            bindings[7].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

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

            // RT output
            bindings[3].binding = 3;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Scene UBO (view/projection inverse matrices for world position reconstruction)
            bindings[4].binding = 4;
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[4].descriptorCount = 1;
            bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Material (PBR parameters from G-buffer)
            bindings[5].binding = 5;
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[5].descriptorCount = 1;
            bindings[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            // Point lights for this frame. Morrowind's interiors are built almost entirely out of
            // these, so without them an interior is a directional sun shining through solid walls.
            bindings[6].binding = 6;
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[6].descriptorCount = 1;
            bindings[6].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mCompositeDescriptorLayout));
        }

        // RT layout: TLAS + storage image + G-buffer samplers
        if (mDevice->rayTracingSupported())
        {
            std::array<VkDescriptorSetLayoutBinding, 16> bindings = {};

            // Bounce albedo history, read (14) and write (15). Same ping-pong rule as bindings 9-12.
            for (uint32_t i = 14; i <= 15; ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            }

            // Ambient occlusion output. Its own target because the RT output has no free channel --
            // .r is sun visibility and .gba is reflected radiance. See rtIndirectFormat.
            bindings[13].binding = 13;
            bindings[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[13].descriptorCount = 1;
            bindings[13].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // Temporal accumulator history, bindings 9-12. Read and write are four separate bindings
            // pointing at four separate images, not two read-write ones -- see the comment on
            // mDenoiseHistory for why a single buffer cannot work.
            //
            // Storage images rather than combined image samplers throughout: the 2x2 history gather is
            // done by hand with four imageLoads and manually renormalised bilinear weights, because a
            // tap that fails the depth or normal test has to contribute exactly zero. A hardware
            // bilinear fetch would have blended it in already.
            for (uint32_t i = 9; i <= 12; ++i)
            {
                bindings[i].binding = i;
                bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                bindings[i].descriptorCount = 1;
                bindings[i].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
            }

            // G-buffer material. raygen needs it to decide whether a reflection ray is worth firing at
            // all: the reflection is weighted by specular strength in composite.frag, and vanilla
            // Morrowind content resolves to zero specular by design, so without this the renderer
            // traces a full-resolution reflection ray per pixel and then multiplies it by nothing.
            bindings[8].binding = 8;
            bindings[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[8].descriptorCount = 1;
            bindings[8].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // Scene UBO. The raygen shader reads its camera matrices and sun direction from here
            // rather than via push constants: two mat4 plus a vec4 is 144 bytes, which exceeds the
            // 128-byte maxPushConstantsSize that Vulkan guarantees on all implementations.
            bindings[5].binding = 5;
            bindings[5].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            bindings[5].descriptorCount = 1;
            bindings[5].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // TLAS
            bindings[0].binding = 0;
            bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            bindings[0].descriptorCount = 1;
            bindings[0].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // Output storage image
            bindings[1].binding = 1;
            bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[1].descriptorCount = 1;
            bindings[1].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // G-buffer albedo
            bindings[2].binding = 2;
            bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[2].descriptorCount = 1;
            bindings[2].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // G-buffer normal
            bindings[3].binding = 3;
            bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[3].descriptorCount = 1;
            bindings[3].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // G-buffer depth
            bindings[4].binding = 4;
            bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[4].descriptorCount = 1;
            bindings[4].stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;

            // Per-instance geometry table, indexed by gl_InstanceCustomIndexEXT. This is what lets the
            // hit shaders reach the triangle they hit: each record carries the vertex and index buffer
            // device addresses plus the texture slot, so any-hit can recover the UV and alpha-test.
            bindings[6].binding = 6;
            bindings[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[6].descriptorCount = 1;
            bindings[6].stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

            // The scene texture array again, this time visible to the hit shaders. It is the same set
            // of views the G-buffer pass samples; writeTextureArrayDescriptors keeps both in step.
            bindings[7].binding = 7;
            bindings[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[7].descriptorCount = maxSceneTextures;
            bindings[7].stageFlags = VK_SHADER_STAGE_ANY_HIT_BIT_KHR | VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mRtDescriptorLayout));
        }
    }

    // Descriptor pool

    void Renderer::createDescriptorPool()
    {
        std::vector<VkDescriptorPoolSize> poolSizes = {
            // maxFramesInFlight * 3: scene set, composite set, and the RT set all reference a UBO.
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxFramesInFlight * 3 },
            // The scene set is allocated per frame in flight and each copy holds the full
            // maxSceneTextures-element sampler array, so the array has to be counted that many times.
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                // The 16 is slack that has absorbed every composite sampler added since: albedo,
                // normal, depth, RT output, material, indirect and the denoise history are seven per
                // composite set, so this is comfortable rather than exact -- unlike the storage image
                // count above, which is exact and does have to be maintained.
                16 + maxFramesInFlight * 4 + maxSceneTextures * maxFramesInFlight },
            // Per RT set: the ray tracing output, the indirect light output, the four denoise history
            // bindings and the two bounce albedo history bindings -- eight. Plus slack. This count is
            // exact rather than generous, so adding a storage image anywhere without raising it fails
            // vkAllocateDescriptorSets with VK_ERROR_OUT_OF_POOL_MEMORY at startup.
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxFramesInFlight * 8 + 2 },
        };

        uint32_t maxSets = maxFramesInFlight * 2 + 4;

        if (mDevice->rayTracingSupported())
        {
            poolSizes.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, maxFramesInFlight });
            // The RT sets carry a second copy of the sampler array (binding 7) for the hit shaders,
            // plus the geometry table.
            poolSizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxFramesInFlight });
        }

        // The composite sets each hold a point light buffer and the scene sets each hold a bone
        // palette, whether or not ray tracing is available. Two per frame in flight.
        {
            auto found = std::find_if(poolSizes.begin(), poolSizes.end(),
                [](const VkDescriptorPoolSize& s) { return s.type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; });
            if (found != poolSizes.end())
                found->descriptorCount += maxFramesInFlight * 3;
            else
                poolSizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxFramesInFlight * 3 });
        }

        if (mDevice->rayTracingSupported())
        {
            for (VkDescriptorPoolSize& size : poolSizes)
            {
                if (size.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
                    size.descriptorCount += maxSceneTextures * maxFramesInFlight;
            }
            maxSets += maxFramesInFlight;
        }

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
        VkDeviceSize bufferSize = sizeof(SceneData);

        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            createBufferLocal(*mDevice, bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mUniformBuffers[i], mUniformMemory[i]);

            VK_CHECK(vmaMapMemory(mDevice->allocator(), mUniformMemory[i], &mUniformMapped[i]));
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
                bufferInfo.range = sizeof(SceneData);

                VkDescriptorBufferInfo skinInfo = {};
                skinInfo.buffer = mSkinBuffers[i];
                skinInfo.offset = 0;
                skinInfo.range = VK_WHOLE_SIZE;

                VkDescriptorBufferInfo particleInfo = {};
                particleInfo.buffer = mParticleBuffers[i];
                particleInfo.offset = 0;
                particleInfo.range = VK_WHOLE_SIZE;

                std::array<VkWriteDescriptorSet, 3> writes = {};

                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = mSceneDescriptorSets[i];
                writes[0].dstBinding = 0;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &bufferInfo;

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = mSceneDescriptorSets[i];
                writes[1].dstBinding = 2;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[1].pBufferInfo = &skinInfo;

                writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[2].dstSet = mSceneDescriptorSets[i];
                writes[2].dstBinding = 3;
                writes[2].descriptorCount = 1;
                writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[2].pBufferInfo = &particleInfo;

                vkUpdateDescriptorSets(
                    mDevice->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            }

            // Binding 1 must be fully populated before the first draw, even though nothing samples it
            // until textures arrive: leaving array elements unwritten is undefined behaviour.
            writeTextureArrayDescriptors();
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
            writeCompositeDescriptor(3, mGBuffer.albedoView);
            writeCompositeDescriptor(5, mGBuffer.materialView);
            // Placeholders, as binding 3 is: every element of the layout must be written before the
            // set is used, whether or not ray tracing is available. The real views are bound below
            // when it is, and without ray tracing neither sample is ever read.
            writeCompositeDescriptor(7, mGBuffer.albedoView);
            writeCompositeDescriptor(8, mGBuffer.albedoView);

            // Bind the scene UBO to each per-frame composite descriptor set
            for (uint32_t i = 0; i < maxFramesInFlight; i++)
            {
                VkDescriptorBufferInfo bufferInfo = {};
                bufferInfo.buffer = mUniformBuffers[i];
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(SceneData);

                VkDescriptorBufferInfo lightInfo = {};
                lightInfo.buffer = mLightBuffers[i];
                lightInfo.offset = 0;
                lightInfo.range = VK_WHOLE_SIZE;

                std::array<VkWriteDescriptorSet, 2> writes = {};

                writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[0].dstSet = mCompositeDescriptorSets[i];
                writes[0].dstBinding = 4;
                writes[0].descriptorCount = 1;
                writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                writes[0].pBufferInfo = &bufferInfo;

                writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[1].dstSet = mCompositeDescriptorSets[i];
                writes[1].dstBinding = 6;
                writes[1].descriptorCount = 1;
                writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[1].pBufferInfo = &lightInfo;

                vkUpdateDescriptorSets(
                    mDevice->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
            }
        }
    }

    void Renderer::writeTextureArrayDescriptors()
    {
        std::vector<VkDescriptorImageInfo> imageInfos(maxSceneTextures);
        for (uint32_t i = 0; i < maxSceneTextures; ++i)
        {
            imageInfos[i].sampler = mTextureSampler;
            imageInfos[i].imageView = mTextureViews[i];
            imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }

        for (uint32_t frame = 0; frame < maxFramesInFlight; ++frame)
        {
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = mSceneDescriptorSets[frame];
            write.dstBinding = 1;
            write.dstArrayElement = 0;
            write.descriptorCount = maxSceneTextures;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = imageInfos.data();

            vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);

            // The hit shaders sample the same array at binding 7 to alpha-test and to look up hit
            // albedo. Written from the same imageInfos so the two can never drift apart.
            if (mRtDescriptorSets[frame] != VK_NULL_HANDLE)
            {
                write.dstSet = mRtDescriptorSets[frame];
                write.dstBinding = 7;
                vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
            }
        }
    }

    void Renderer::setTextures(const std::vector<VkImageView>& views)
    {
        const VkImageView fallback = mFallbackTexture->view();

        // Slot 0 is the fallback, so only maxSceneTextures - 1 real textures fit.
        const size_t usable = std::min<size_t>(views.size(), maxSceneTextures - 1);
        if (views.size() > usable && !mTextureOverflowWarned)
        {
            mTextureOverflowWarned = true;
            Log(Debug::Warning) << "Vulkan: " << views.size() << " scene textures exceed the "
                                << (maxSceneTextures - 1) << " sampler array slots available; the excess "
                                << "will render with the fallback texture";
        }

        mTextureViews.assign(maxSceneTextures, fallback);
        for (size_t i = 0; i < usable; ++i)
            mTextureViews[i + 1] = views[i] != VK_NULL_HANDLE ? views[i] : fallback;

        // Descriptor sets are per frame in flight and may still be bound by a submitted command
        // buffer. Rewriting the array is rare (cell load only), so idling is cheap enough.
        vkDeviceWaitIdle(mDevice->handle());
        writeTextureArrayDescriptors();
    }

    // RT output image

    void Renderer::createRtOutput()
    {
        VkExtent2D extent = mSwapchain->extent();

        createImage(extent.width, extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            mRtOutput.image, mRtOutput.memory);
        mRtOutput.view = createImageView(mRtOutput.image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

        createImage(extent.width, extent.height, rtIndirectFormat,
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            mRtIndirect.image, mRtIndirect.memory);
        mRtIndirect.view = createImageView(mRtIndirect.image, rtIndirectFormat, VK_IMAGE_ASPECT_COLOR_BIT);

        VkCommandBuffer cmd = mCommandPool->beginSingleTime();

        // Clear rather than just transitioning. composite.frag samples this image unconditionally, but
        // the RT pass is skipped whenever there is no TLAS -- during startup before the first cell is
        // loaded, and for any cell whose instances all lack a BLAS. Without the clear those frames read
        // undefined memory as the shadow term, which shows up as a scene randomly lit or unlit.
        // (1, 0, 0, 0) is what raygen writes for a sky pixel: fully lit, no reflection.
        transitionImageLayout(cmd, mRtOutput.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkClearColorValue clearColor = {};
        clearColor.float32[0] = 1.0f;
        VkImageSubresourceRange range = {};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(
            cmd, mRtOutput.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

        transitionImageLayout(cmd, mRtOutput.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        // Same treatment for the indirect light target, and for the same reason: the composite pass
        // samples it unconditionally but the RT pass is skipped whenever there is no TLAS.
        //
        // Cleared to (0, 0, 0, 1): no bounce colour, fully sky-visible. That makes a frame without
        // ray tracing look like the renderer did before any of this existed -- no indirect term and
        // no occlusion -- rather than like the world is sealed inside a black box.
        transitionImageLayout(cmd, mRtIndirect.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkClearColorValue indirectClear = {};
        indirectClear.float32[3] = 1.0f;
        vkCmdClearColorImage(
            cmd, mRtIndirect.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &indirectClear, 1, &range);

        transitionImageLayout(cmd, mRtIndirect.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        mCommandPool->endSingleTime(cmd, mDevice->graphicsQueue());
    }

    void Renderer::destroyRtOutput()
    {
        VkDevice dev = mDevice->handle();
        for (RtOutputImage* target : { &mRtOutput, &mRtIndirect })
        {
            if (target->view != VK_NULL_HANDLE)
            {
                vkDestroyImageView(dev, target->view, nullptr);
                target->view = VK_NULL_HANDLE;
            }
            if (target->image != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mDevice->allocator(), target->image, target->memory);
                target->image = VK_NULL_HANDLE;
                target->memory = VK_NULL_HANDLE;
            }
        }
    }

    void Renderer::createDenoiseTargets()
    {
        VkExtent2D extent = mSwapchain->extent();

        VkCommandBuffer cmd = mCommandPool->beginSingleTime();

        // SAMPLED_BIT so composite.frag can read the history length its spatial fallback is gated on.
        // The warning that used to be here still stands in spirit: raygen's 2x2 history gather must
        // keep using imageLoad, because a hardware bilinear fetch blends rejected taps back in before
        // the shader can test them. Sampling for a scalar the fragment shader only reads 1:1 is a
        // different thing entirely.
        const VkImageUsageFlags usage
            = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        for (uint32_t i = 0; i < maxFramesInFlight; ++i)
        {
            const std::array<std::pair<DenoiseTarget*, VkFormat>, 3> targets = {
                std::make_pair(&mDenoiseHistory[i], denoiseHistoryFormat),
                std::make_pair(&mDenoiseGeom[i], denoiseGeomFormat),
                std::make_pair(&mDenoiseIndirect[i], rtIndirectFormat)
            };

            for (const auto& [target, format] : targets)
            {
                createImage(extent.width, extent.height, format, usage, target->image, target->memory);
                target->view = createImageView(target->image, format, VK_IMAGE_ASPECT_COLOR_BIT);

                // Clearing is not optional, and the failure mode is worse than the one that made the
                // RT output's clear necessary. Uninitialised memory read as a history length either
                // pins the blend weight at its floor over garbage forever, or -- if the bit pattern
                // decodes to a NaN -- poisons the accumulator permanently, because a NaN multiplies
                // into the next frame's history and never washes out.
                //
                // All zeros is the correct initial state for both images and needs no special case in
                // the shader. Zero history length forces a blend weight of 1, so the first frame is a
                // straight copy of the raw signal and the mean and second moment self-heal. Zero in
                // the geometry image is a stored view-space Z of exactly 0.0, which fails the depth
                // test against any real surface, so a stale tap can never be accepted.
                transitionImageLayout(cmd, target->image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

                VkClearColorValue clearColor = {};
                VkImageSubresourceRange range = {};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.levelCount = 1;
                range.layerCount = 1;
                vkCmdClearColorImage(
                    cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

                // GENERAL for the rest of their lifetime. They are only ever touched by raygen, as
                // storage images, in both directions.
                transitionImageLayout(cmd, target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }

        mCommandPool->endSingleTime(cmd, mDevice->graphicsQueue());
    }

    void Renderer::destroyDenoiseTargets()
    {
        VkDevice dev = mDevice->handle();

        for (uint32_t i = 0; i < maxFramesInFlight; ++i)
        {
            for (DenoiseTarget* target : { &mDenoiseHistory[i], &mDenoiseGeom[i], &mDenoiseIndirect[i] })
            {
                if (target->view != VK_NULL_HANDLE)
                {
                    vkDestroyImageView(dev, target->view, nullptr);
                    target->view = VK_NULL_HANDLE;
                }
                if (target->image != VK_NULL_HANDLE)
                {
                    vmaDestroyImage(mDevice->allocator(), target->image, target->memory);
                    target->image = VK_NULL_HANDLE;
                    target->memory = VK_NULL_HANDLE;
                }
            }
        }
    }

    void Renderer::createRtDescriptorSets()
    {
        if (mRtDescriptorSets[0] == VK_NULL_HANDLE)
        {
            std::array<VkDescriptorSetLayout, maxFramesInFlight> layouts;
            layouts.fill(mRtDescriptorLayout);

            VkDescriptorSetAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            allocInfo.descriptorPool = mDescriptorPool;
            allocInfo.descriptorSetCount = maxFramesInFlight;
            allocInfo.pSetLayouts = layouts.data();

            VK_CHECK(vkAllocateDescriptorSets(mDevice->handle(), &allocInfo, mRtDescriptorSets.data()));
        }

        auto makeImageInfo = [&](VkImageView view) {
            VkDescriptorImageInfo info = {};
            info.sampler = mGBufferSampler;
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            return info;
        };

        VkDescriptorImageInfo storageImageInfo = {};
        storageImageInfo.imageView = mRtOutput.view;
        storageImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        const std::array<VkDescriptorImageInfo, 3> gbufferInfos = {
            makeImageInfo(mGBuffer.albedoView),
            makeImageInfo(mGBuffer.normalView),
            makeImageInfo(mGBuffer.depthView)
        };

        const VkDescriptorImageInfo materialInfo = makeImageInfo(mGBuffer.materialView);

        auto makeStorageInfo = [](VkImageView view) {
            VkDescriptorImageInfo info = {};
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return info;
        };

        // mCurrentFrame alternates 0, 1, 0, 1..., so the set bound on frame N reads exactly what the
        // set bound on frame N-1 wrote. That is only true because there are two frames in flight. At
        // three the parity trick breaks -- set 0 would read set 1's output, but the previous frame was
        // set 2 -- and it breaks silently, into a denoiser that mostly works and occasionally
        // accumulates two-frame-old history. If this ever changes, the read index must become
        // (frame + maxFramesInFlight - 1) % maxFramesInFlight.
        static_assert(maxFramesInFlight == 2, "denoise history ping-pong assumes two frames in flight");

        for (uint32_t frame = 0; frame < maxFramesInFlight; ++frame)
        {
            const uint32_t prevFrame = 1 - frame;

            VkDescriptorBufferInfo sceneInfo = {};
            sceneInfo.buffer = mUniformBuffers[frame];
            sceneInfo.offset = 0;
            sceneInfo.range = sizeof(SceneData);

            const std::array<VkDescriptorImageInfo, 4> denoiseInfos = {
                makeStorageInfo(mDenoiseHistory[prevFrame].view), // binding  9: signal, read
                makeStorageInfo(mDenoiseHistory[frame].view),     // binding 10: signal, write
                makeStorageInfo(mDenoiseGeom[prevFrame].view),    // binding 11: geometry, read
                makeStorageInfo(mDenoiseGeom[frame].view)         // binding 12: geometry, write
            };

            const std::array<VkDescriptorImageInfo, 2> indirectInfos = {
                makeStorageInfo(mDenoiseIndirect[prevFrame].view), // binding 14: bounce, read
                makeStorageInfo(mDenoiseIndirect[frame].view)      // binding 15: bounce, write
            };

            const VkDescriptorImageInfo indirectStorageInfo = makeStorageInfo(mRtIndirect.view);

            std::array<VkWriteDescriptorSet, 13> writes = {};

            // Binding 13: indirect light output (bounce albedo + sky visibility)
            writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[10].dstSet = mRtDescriptorSets[frame];
            writes[10].dstBinding = 13;
            writes[10].descriptorCount = 1;
            writes[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[10].pImageInfo = &indirectStorageInfo;

            // Bindings 14-15: bounce albedo history, read and write
            for (uint32_t i = 0; i < 2; ++i)
            {
                writes[i + 11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i + 11].dstSet = mRtDescriptorSets[frame];
                writes[i + 11].dstBinding = 14 + i;
                writes[i + 11].descriptorCount = 1;
                writes[i + 11].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i + 11].pImageInfo = &indirectInfos[i];
            }

            // Binding 1: output storage image
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = mRtDescriptorSets[frame];
            writes[0].dstBinding = 1;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[0].pImageInfo = &storageImageInfo;

            // Bindings 2-4: G-buffer samplers
            for (uint32_t i = 0; i < 3; ++i)
            {
                writes[i + 1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i + 1].dstSet = mRtDescriptorSets[frame];
                writes[i + 1].dstBinding = 2 + i;
                writes[i + 1].descriptorCount = 1;
                writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i + 1].pImageInfo = &gbufferInfos[i];
            }

            // Binding 5: scene UBO (camera matrices + sun direction)
            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[4].dstSet = mRtDescriptorSets[frame];
            writes[4].dstBinding = 5;
            writes[4].descriptorCount = 1;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[4].pBufferInfo = &sceneInfo;

            // Binding 8: G-buffer material, so raygen can skip the reflection ray on surfaces with no
            // specular response -- which is all vanilla Morrowind content.
            writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[5].dstSet = mRtDescriptorSets[frame];
            writes[5].dstBinding = 8;
            writes[5].descriptorCount = 1;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[5].pImageInfo = &materialInfo;

            // Bindings 9-12: the temporal accumulator's history, written once here rather than per
            // frame. Which image is read and which is written is baked into the set, so binding the
            // right set is all the frame loop has to do.
            for (uint32_t i = 0; i < 4; ++i)
            {
                writes[i + 6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i + 6].dstSet = mRtDescriptorSets[frame];
                writes[i + 6].dstBinding = 9 + i;
                writes[i + 6].descriptorCount = 1;
                writes[i + 6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i + 6].pImageInfo = &denoiseInfos[i];
            }

            vkUpdateDescriptorSets(
                mDevice->handle(), static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        }

        // Binding 7, the hit shaders' copy of the sampler array, has to be written here rather than
        // relying on the next setTextures(). This runs *after* createDescriptorSets(), so the RT sets
        // did not exist when writeTextureArrayDescriptors last ran and it skipped them; and
        // syncTexturesToRenderer early-returns when no new texture was loaded, so a cell whose textures
        // all fail to resolve would reach vkCmdTraceRaysKHR with 512 descriptors never written. The
        // layout does not set PARTIALLY_BOUND, so that is undefined behaviour rather than reads of black.
        writeTextureArrayDescriptors();
    }


    // Pipelines

    void Renderer::createGBufferPipeline()
    {
        VkPushConstantRange pushConstant = {};
        // The vertex stage reads model/normalMatrix and the fragment stage reads textureIndex out of
        // the same block, so the range has to cover both stages.
        pushConstant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushConstant.offset = 0;
        pushConstant.size = sizeof(GBufferPushConstants);

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
        // sunDirection, sunColor, cameraPosition, ambientColor. 64 bytes, half the guaranteed limit.
        pushConstant.size = sizeof(Vec4) * 4;
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
        // Not in the check below. A build without it still renders everything, actors included, just
        // without the per-vertex pose.
        auto gbufSkinnedVert = loadShader("gbuffer_skinned.vert.spv");
        auto compVert = loadShader("composite.vert.spv");
        auto compFrag = loadShader("composite.frag.spv");
        // Also not in the check below. Without these the world renders and the effects do not.
        auto particleVert = loadShader("particle.vert.spv");
        auto particleFrag = loadShader("particle.frag.spv");

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

            // The same pipeline again with the skinning vertex shader and a second vertex binding.
            // Everything else is deliberately shared with the block above, including the layout and
            // the render pass, so the two cannot drift in any respect that matters to the G-buffer.
            //
            // Skipped rather than fatal if its shader is missing: the renderer then draws actors in
            // bind pose, which is what it did before skinning existed.
            if (gbufSkinnedVert)
            {
                std::array<VkPipelineShaderStageCreateInfo, 2> skinnedStages = {
                    gbufSkinnedVert->stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    gbufFrag->stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT)
                };

                std::array<VkVertexInputBindingDescription, 2> skinnedBindingDesc = {};
                skinnedBindingDesc[0] = bindingDesc[0];
                skinnedBindingDesc[1].binding = 1;
                skinnedBindingDesc[1].stride = 8;
                skinnedBindingDesc[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

                std::array<VkVertexInputAttributeDescription, 6> skinnedAttrDesc = {};
                for (size_t i = 0; i < attrDesc.size(); ++i)
                    skinnedAttrDesc[i] = attrDesc[i];
                // Indices stay integers; weights are unorm bytes and arrive in the shader as floats
                // already divided by 255, which is what makes four bytes enough to carry them.
                skinnedAttrDesc[4] = { 4, 1, VK_FORMAT_R8G8B8A8_UINT, 0 };
                skinnedAttrDesc[5] = { 5, 1, VK_FORMAT_R8G8B8A8_UNORM, 4 };

                VkPipelineVertexInputStateCreateInfo skinnedVertexInput = vertexInput;
                skinnedVertexInput.vertexBindingDescriptionCount
                    = static_cast<uint32_t>(skinnedBindingDesc.size());
                skinnedVertexInput.pVertexBindingDescriptions = skinnedBindingDesc.data();
                skinnedVertexInput.vertexAttributeDescriptionCount
                    = static_cast<uint32_t>(skinnedAttrDesc.size());
                skinnedVertexInput.pVertexAttributeDescriptions = skinnedAttrDesc.data();

                VkGraphicsPipelineCreateInfo skinnedInfo = pipelineInfo;
                skinnedInfo.pStages = skinnedStages.data();
                skinnedInfo.pVertexInputState = &skinnedVertexInput;

                VK_CHECK(vkCreateGraphicsPipelines(mDevice->handle(), VK_NULL_HANDLE, 1,
                    &skinnedInfo, nullptr, &mGBufferSkinnedPipeline));
            }
        }

        // Particle pipeline. Draws inside the composite render pass, after the tone mapped scene and
        // before the interface, so it needs no render pass and no framebuffer of its own.
        if (particleVert && particleFrag)
        {
            std::array<VkPipelineShaderStageCreateInfo, 2> stages
                = { particleVert->stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                      particleFrag->stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT) };

            // Two sets: the scene set for the camera, the sampler array and the particle buffer, and
            // the composite set for the G-buffer depth. Both are already allocated per frame in
            // flight and already bound with the right contents; reusing them is what keeps this pass
            // free of descriptor plumbing.
            std::array<VkDescriptorSetLayout, 2> setLayouts
                = { mSceneDescriptorLayout, mCompositeDescriptorLayout };

            VkPipelineLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = static_cast<uint32_t>(setLayouts.size());
            layoutInfo.pSetLayouts = setLayouts.data();
            VK_CHECK(vkCreatePipelineLayout(
                mDevice->handle(), &layoutInfo, nullptr, &mParticlePipelineLayout));

            // No vertex input: the quad corner comes from gl_VertexIndex and the particle from
            // gl_InstanceIndex.
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
            // A billboard has no meaningful winding -- it faces the camera by construction, and which
            // way round its two triangles come out depends on where the camera is.
            rasterizer.cullMode = VK_CULL_MODE_NONE;

            VkPipelineMultisampleStateCreateInfo multisampling = {};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

            // The composite pass has no depth attachment, so there is nothing to test against here.
            // particle.frag samples the G-buffer depth instead.
            VkPipelineDepthStencilStateCreateInfo depthStencil = {};
            depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
            depthStencil.depthTestEnable = VK_FALSE;
            depthStencil.depthWriteEnable = VK_FALSE;

            // Additive. Order independent by construction, which is why this needs no sorting.
            VkPipelineColorBlendAttachmentState blend = {};
            blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blend.blendEnable = VK_TRUE;
            blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.colorBlendOp = VK_BLEND_OP_ADD;
            blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            blend.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending = {};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &blend;

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
            pipelineInfo.layout = mParticlePipelineLayout;
            pipelineInfo.renderPass = mCompositeRenderPass;
            pipelineInfo.subpass = 0;

            VK_CHECK(vkCreateGraphicsPipelines(
                mDevice->handle(), VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mParticlePipeline));
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

        if (mRayTracingEnabled)
        {
            auto raygen = loadShader("raygen.rgen.spv");
            auto miss = loadShader("miss.rmiss.spv");
            auto shadowMiss = loadShader("shadow.rmiss.spv");
            auto closestHit = loadShader("closesthit.rchit.spv");
            auto anyHit = loadShader("anyhit.rahit.spv");

            if (raygen && miss && shadowMiss && closestHit && anyHit)
            {
                mRtPipeline = std::make_unique<RayTracingPipeline>(
                    *mDevice, mRtDescriptorLayout, *raygen, *miss, *shadowMiss, *closestHit, *anyHit);
                Log(Debug::Info) << "Vulkan ray tracing pipeline created";
            }
            else
            {
                Log(Debug::Warning) << "Vulkan ray tracing shaders not found in " << shaderDir
                                    << ", ray tracing disabled";
            }
        }

        Log(Debug::Info) << "Vulkan pipelines created successfully";
        return true;
    }

    // Frame lifecycle

    uint32_t Renderer::beginFrame()
    {
        mFrameSync->waitForFrame(mCurrentFrame);

        VkResult result;
        for (;;)
        {
            result = vkAcquireNextImageKHR(mDevice->handle(), mSwapchain->handle(),
                UINT64_MAX, mFrameSync->imageAvailable(mCurrentFrame), VK_NULL_HANDLE, &mCurrentImageIndex);

            if (result == VK_ERROR_OUT_OF_DATE_KHR)
            {
                int w, h;
                getDrawableSize(mWindow, w, h);
                resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                continue;
            }
            break;
        }

        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
            VK_CHECK(result);

        mFrameSync->resetFrame(mCurrentFrame);

        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        vkResetCommandBuffer(cmd, 0);

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));

        return mCurrentFrame;
    }

    void Renderer::endFrame()
    {
        VkCommandBuffer cmd = mCommandBuffers[mCurrentFrame];
        VK_CHECK(vkEndCommandBuffer(cmd));

        VkSemaphore waitSemaphore = mFrameSync->imageAvailable(mCurrentFrame);
        // Indexed by acquired image, not frame in flight -- the presentation engine may still be
        // waiting on this semaphore from an earlier present of the same image.
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
            getDrawableSize(mWindow, w, h);
            resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
        else
        {
            // Anything else -- device lost, surface lost, out of device memory -- must not be swallowed.
            // Without this the loop keeps submitting into a dead device and the real error surfaces
            // later somewhere unrelated. The acquire path already does this; present did not.
            VK_CHECK(result);
        }

        mCurrentFrame = (mCurrentFrame + 1) % maxFramesInFlight;
    }

    void Renderer::render()
    {
        // Before beginFrame(): buildTlas() idles the device, which must not happen mid-frame.
        if (mRayTracingEnabled && mTlasDirty)
            buildTlas();

        beginFrame();

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

                // Which of the two G-buffer pipelines is currently bound. Tracked rather than sorting
                // the draws, because they already arrive grouped -- cell geometry, then actors, then
                // terrain -- so this costs a couple of binds a frame and keeps submission order,
                // which the caller relies on for nothing but is easier to reason about.
                VkPipeline boundPipeline = mGBufferPipeline;

                for (const auto& drawCmd : mDrawCommands)
                {
                    // Raster only. buildTlas deliberately does not check this -- see MeshSubmission.
                    if (!drawCmd.visible)
                        continue;

                    const bool skinned = mGBufferSkinnedPipeline != VK_NULL_HANDLE
                        && drawCmd.skinBuffer != VK_NULL_HANDLE && drawCmd.boneOffset != sNoBones;
                    const VkPipeline wanted = skinned ? mGBufferSkinnedPipeline : mGBufferPipeline;
                    if (wanted != boundPipeline)
                    {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, wanted);
                        boundPipeline = wanted;
                    }

                    GBufferPushConstants pushData = {};
                    pushData.model = drawCmd.transform;
                    // Pack the upper-left 3x3 of the normal matrix into the shader's mat3, which lays
                    // its three columns out at 16-byte stride. Element (row, col) of a column-major
                    // Mat4 is at data[col * 4 + row], so each column's first three floats copy across
                    // directly and the fourth is pure padding.
                    for (int col = 0; col < 3; ++col)
                    {
                        pushData.normalMatrix[col * 4 + 0] = drawCmd.normalMatrix.data[col * 4 + 0];
                        pushData.normalMatrix[col * 4 + 1] = drawCmd.normalMatrix.data[col * 4 + 1];
                        pushData.normalMatrix[col * 4 + 2] = drawCmd.normalMatrix.data[col * 4 + 2];
                        pushData.normalMatrix[col * 4 + 3] = 0.0f;
                    }
                    pushData.textureIndex = drawCmd.textureIndex;
                    pushData.roughness = drawCmd.roughness;
                    pushData.specularStrength = drawCmd.specularStrength;
                    pushData.boneOffset = skinned ? drawCmd.boneOffset : sNoBones;

                    vkCmdPushConstants(cmd, mGBufferPipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(pushData), &pushData);

                    if (skinned)
                    {
                        const std::array<VkBuffer, 2> buffers = { drawCmd.vertexBuffer, drawCmd.skinBuffer };
                        const std::array<VkDeviceSize, 2> offsets = { 0, 0 };
                        vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
                    }
                    else
                    {
                        VkDeviceSize offset = 0;
                        vkCmdBindVertexBuffers(cmd, 0, 1, &drawCmd.vertexBuffer, &offset);
                    }
                    vkCmdBindIndexBuffer(cmd, drawCmd.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, drawCmd.indexCount, 1, 0, 0, 0);
                }
            }

            vkCmdEndRenderPass(cmd);
        }

        // 2. Ray tracing pass (shadows + reflections)
        if (mRayTracingEnabled && mRtPipeline && mTlas)
        {
            // The denoise history stays in GENERAL for its whole lifetime, so these are pure memory
            // dependencies rather than layout changes -- see the GENERAL -> GENERAL branch of
            // transitionImageLayout for why they are not free and not optional.
            //
            // Read side: last frame's raygen wrote the images this frame's raygen is about to read.
            // Write side: a write-after-read against frame N-2, which today the frame fence already
            // covers, but that is a consequence of maxFramesInFlight being 2 rather than anything
            // this code states, and the barrier costs nothing to include.
            {
                std::array<VkImageMemoryBarrier, 6> denoiseBarriers = {};
                const std::array<VkImage, 6> denoiseImages = {
                    mDenoiseHistory[1 - mCurrentFrame].image, mDenoiseGeom[1 - mCurrentFrame].image,
                    mDenoiseIndirect[1 - mCurrentFrame].image,
                    mDenoiseHistory[mCurrentFrame].image, mDenoiseGeom[mCurrentFrame].image,
                    mDenoiseIndirect[mCurrentFrame].image
                };

                for (size_t i = 0; i < denoiseBarriers.size(); ++i)
                {
                    VkImageMemoryBarrier& barrier = denoiseBarriers[i];
                    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    barrier.image = denoiseImages[i];
                    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    barrier.subresourceRange.levelCount = 1;
                    barrier.subresourceRange.layerCount = 1;
                }

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0, nullptr, 0, nullptr,
                    static_cast<uint32_t>(denoiseBarriers.size()), denoiseBarriers.data());
            }

            transitionImageLayout(cmd, mRtOutput.image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
            transitionImageLayout(cmd, mRtIndirect.image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_ASPECT_COLOR_BIT);

            mRtPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                mRtPipeline->layout(), 0, 1, &mRtDescriptorSets[mCurrentFrame], 0, nullptr);

            mRtPipeline->traceRays(cmd, extent.width, extent.height);

            transitionImageLayout(cmd, mRtOutput.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
            transitionImageLayout(cmd, mRtIndirect.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);

            // composite.frag samples this frame's history for the per-pixel history length. It stays
            // in GENERAL -- sampling from GENERAL is legal and avoids two layout transitions per
            // frame on an image whose whole point is never changing layout -- so this is a pure
            // memory dependency from raygen's writes to the fragment shader's reads. Without it the
            // spatial filter would gate on whatever was in the image last frame, which would fail
            // intermittently and only while the camera moves.
            {
                VkImageMemoryBarrier barrier = {};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.image = mDenoiseHistory[mCurrentFrame].image;
                barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barrier.subresourceRange.levelCount = 1;
                barrier.subresourceRange.layerCount = 1;

                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
        }

        // 3. Composite pass
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

                // Push sun direction, sun color, and camera position. Read from the CPU-side copy, not
                // back out of the mapped uniform buffer: that memory is written sequentially and may be
                // write-combined, where host reads are uncached and far slower than they look.
                const SceneData* sceneData = &mCurrentScene;
                Vec4 cameraPos;
                cameraPos.x = sceneData->viewInverse.data[12];
                cameraPos.y = sceneData->viewInverse.data[13];
                cameraPos.z = sceneData->viewInverse.data[14];
                cameraPos.w = 1.0f;

                std::array<Vec4, 4> compositePush
                    = { sceneData->sunDirection, sceneData->sunColor, cameraPos, sceneData->ambientColor };
                vkCmdPushConstants(cmd, mCompositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(Vec4) * 4, compositePush.data());

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }

            // Particle effects -- fire, smoke, sparks, spell effects. In this pass rather than the
            // G-buffer because they are blended, and a G-buffer has nowhere to put a translucent
            // surface. After the tone mapped scene so they add light to a finished image, and before
            // the interface so the interface stays on top.
            //
            // Additive, so no sorting is needed: addition commutes, and the draw order of overlapping
            // flames cannot change the result. That is the whole reason this needs no transparency
            // architecture.
            if (mParticlePipeline != VK_NULL_HANDLE && mParticleCount > 0)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mParticlePipeline);

                std::array<VkDescriptorSet, 2> sets
                    = { mSceneDescriptorSets[mCurrentFrame], mCompositeDescriptorSets[mCurrentFrame] };
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mParticlePipelineLayout, 0,
                    static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);

                // Six vertices a quad, one instance a particle. No vertex buffer: the corner comes
                // from gl_VertexIndex and the particle from gl_InstanceIndex.
                vkCmdDraw(cmd, 6, mParticleCount, 0, 0);
            }

            // The user interface draws here, sharing the composite pass rather than running one of its
            // own. It blends straight onto the tone mapped scene in the swapchain image, so there is no
            // second attachment, no extra layout transition and nothing to resolve. Outside the
            // pipeline check above deliberately: if the composite shaders failed to load the UI should
            // still appear, which is also what makes a black window with a working menu diagnosable.
            if (mOverlayCallback)
                mOverlayCallback(cmd, mCurrentFrame, extent);

            vkCmdEndRenderPass(cmd);
        }

        // Inside the frame, while the image is still ours. See requestScreenshot for why it cannot
        // be done after the present instead.
        if (mScreenshotRequested)
        {
            mScreenshotRequested = false;
            recordScreenshotCopy(cmd, extent);
        }

        mDrawCommands.clear();

        endFrame();

        if (mScreenshotPending)
            resolveScreenshot();
    }

    void Renderer::resize(uint32_t width, uint32_t height)
    {
        vkDeviceWaitIdle(mDevice->handle());

        // The swapchain images and the readback buffer's size both belong to the old extent.
        mScreenshotPending = false;
        mScreenshotBuffer.reset();
        mScreenshotBufferSize = 0;

        if (mGBufferFramebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(mDevice->handle(), mGBufferFramebuffer, nullptr);
            mGBufferFramebuffer = VK_NULL_HANDLE;
        }
        destroyCompositeFramebuffers();
        destroyGBuffer();

        if (mRayTracingEnabled)
        {
            destroyRtOutput();
            destroyDenoiseTargets();
        }

        mSwapchain->recreate(width, height);
        mFrameSync->resizeImageSemaphores(static_cast<uint32_t>(mSwapchain->imageViews().size()));

        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();

        if (mRayTracingEnabled)
        {
            createRtOutput();
            // Recreated at the new extent and re-cleared, so the accumulator restarts from an age of
            // zero everywhere. Do not "optimise" the clear away on the grounds that fresh VMA memory
            // is usually zero -- a resize would then hand the accumulator a full screen of garbage
            // history lengths, and one NaN in there never washes out.
            createDenoiseTargets();
            createRtDescriptorSets();
        }

        writeCompositeDescriptor(0, mGBuffer.albedoView);
        writeCompositeDescriptor(1, mGBuffer.normalView);
        writeCompositeDescriptor(2, mGBuffer.depthView);
        writeCompositeDescriptor(5, mGBuffer.materialView);

        if (mRtOutput.view != VK_NULL_HANDLE)
        {
            writeCompositeDescriptor(3, mRtOutput.view);
            writeCompositeDescriptor(7, mRtIndirect.view);
            writeCompositeHistoryDescriptors();
        }
        else
        {
            writeCompositeDescriptor(3, mGBuffer.albedoView);
            writeCompositeDescriptor(7, mGBuffer.albedoView);
            writeCompositeDescriptor(8, mGBuffer.albedoView);
        }
    }

    void Renderer::createLightBuffers()
    {
        const VkDeviceSize size = sizeof(PointLight) * maxPointLights;
        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            createBufferLocal(*mDevice, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mLightBuffers[i], mLightMemory[i]);
            VK_CHECK(vmaMapMemory(mDevice->allocator(), mLightMemory[i], &mLightMapped[i]));
        }
    }

    void Renderer::createSkinBuffers()
    {
        const VkDeviceSize size = sizeof(float) * 16 * maxSkinMatrices;
        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            createBufferLocal(*mDevice, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mSkinBuffers[i], mSkinMemory[i]);
            VK_CHECK(vmaMapMemory(mDevice->allocator(), mSkinMemory[i], &mSkinMapped[i]));
        }
    }

    void Renderer::createParticleBuffers()
    {
        const VkDeviceSize size = sizeof(ParticleQuad) * maxParticleQuads;
        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            createBufferLocal(*mDevice, size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mParticleBuffers[i], mParticleMemory[i]);
            VK_CHECK(vmaMapMemory(mDevice->allocator(), mParticleMemory[i], &mParticleMapped[i]));
        }
    }

    void Renderer::updateParticles(const ParticleQuad* quads, uint32_t count)
    {
        const uint32_t usable = std::min(count, maxParticleQuads);
        if (count > usable && !mParticleOverflowWarned)
        {
            mParticleOverflowWarned = true;
            Log(Debug::Warning) << "Vulkan: " << count << " particle quads exceed the " << maxParticleQuads
                                << " one frame can hold; the rest are dropped";
        }

        if (usable > 0 && quads != nullptr && mParticleMapped[mCurrentFrame] != nullptr)
            std::memcpy(mParticleMapped[mCurrentFrame], quads, sizeof(ParticleQuad) * usable);

        mParticleCount = usable;
    }

    uint32_t Renderer::updateSkinMatrices(const float* matrices, uint32_t count)
    {
        const uint32_t usable = std::min(count, maxSkinMatrices);
        if (count > usable && !mSkinOverflowWarned)
        {
            mSkinOverflowWarned = true;
            Log(Debug::Warning) << "Vulkan: " << count << " bone matrices exceed the " << maxSkinMatrices
                                << " one frame can hold; the shapes past that are drawn in bind pose";
        }

        if (usable > 0 && matrices != nullptr && mSkinMapped[mCurrentFrame] != nullptr)
            std::memcpy(mSkinMapped[mCurrentFrame], matrices, sizeof(float) * 16 * usable);

        return usable;
    }

    void Renderer::updateLights(const PointLight* lights, uint32_t count)
    {
        const uint32_t usable = std::min(count, maxPointLights);
        if (count > usable && !mLightOverflowWarned)
        {
            mLightOverflowWarned = true;
            Log(Debug::Warning) << "Vulkan: " << count << " point lights exceed the " << maxPointLights
                                << " the composite pass can hold; the rest are dropped";
        }

        if (usable > 0 && lights != nullptr && mLightMapped[mCurrentFrame] != nullptr)
            std::memcpy(mLightMapped[mCurrentFrame], lights, sizeof(PointLight) * usable);

        mLightCount = usable;
    }

    void Renderer::waitIdle()
    {
        vkDeviceWaitIdle(mDevice->handle());
    }

    void Renderer::recordScreenshotCopy(VkCommandBuffer cmd, VkExtent2D extent)
    {
        if (!mSwapchain || extent.width == 0 || extent.height == 0)
            return;

        const std::vector<VkImage>& images = mSwapchain->images();
        if (mCurrentImageIndex >= images.size())
            return;

        const VkDeviceSize byteCount = VkDeviceSize(extent.width) * extent.height * 4;
        if (!mScreenshotBuffer || mScreenshotBufferSize != byteCount)
        {
            // Safe to replace here rather than retiring it: the only submission that ever reads or
            // writes this buffer is the one being recorded, and the previous screenshot's submission
            // was waited on in resolveScreenshot before this could be reached again.
            mScreenshotBuffer = std::make_unique<Buffer>(*mDevice, byteCount, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            mScreenshotBufferSize = byteCount;
        }

        VkImage image = images[mCurrentImageIndex];

        // The composite render pass leaves the image in PRESENT_SRC, and it has to be handed back in
        // that layout because endFrame presents it straight afterwards.
        transitionImageLayout(
            cmd, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        VkBufferImageCopy region = {};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { 0, 0, 0 };
        region.imageExtent = { extent.width, extent.height, 1 };
        vkCmdCopyImageToBuffer(
            cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mScreenshotBuffer->handle(), 1, &region);

        transitionImageLayout(
            cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);

        mScreenshotWidth = extent.width;
        mScreenshotHeight = extent.height;
        mScreenshotPending = true;
    }

    void Renderer::resolveScreenshot()
    {
        mScreenshotPending = false;

        if (!mScreenshotBuffer)
            return;

        // The copy is part of the frame's submission, so waiting for the device to go idle is enough
        // to know it has landed. Blunt, and this only runs when someone pressed the screenshot key.
        vkDeviceWaitIdle(mDevice->handle());

        const uint8_t* mapped = static_cast<const uint8_t*>(mScreenshotBuffer->map());
        if (mapped == nullptr)
            return;

        // chooseSurfaceFormat asks for B8G8R8A8_SRGB and warns when it cannot have it, so the channel
        // order is known rather than guessed -- but it is checked rather than swizzled blind, because
        // on a fallback format the alternative is a screenshot with red and blue swapped, which reads
        // as a shader bug.
        const bool swapRedBlue
            = mSwapchain->format() == VK_FORMAT_B8G8R8A8_SRGB || mSwapchain->format() == VK_FORMAT_B8G8R8A8_UNORM;

        mScreenshotPixels.resize(static_cast<size_t>(mScreenshotBufferSize));
        for (size_t i = 0; i < mScreenshotPixels.size(); i += 4)
        {
            mScreenshotPixels[i + 0] = swapRedBlue ? mapped[i + 2] : mapped[i + 0];
            mScreenshotPixels[i + 1] = mapped[i + 1];
            mScreenshotPixels[i + 2] = swapRedBlue ? mapped[i + 0] : mapped[i + 2];
            // The swapchain has no meaningful alpha -- the composite pass writes 1.0 -- but a
            // screenshot with a zero alpha channel opens as fully transparent in most viewers, which
            // looks exactly like a renderer that drew nothing.
            mScreenshotPixels[i + 3] = 0xff;
        }

        mScreenshotBuffer->unmap();
        mScreenshotReady = true;
    }

    bool Renderer::takeScreenshot(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
    {
        if (!mScreenshotReady)
            return false;

        rgba = std::move(mScreenshotPixels);
        mScreenshotPixels.clear();
        width = mScreenshotWidth;
        height = mScreenshotHeight;
        mScreenshotReady = false;
        return true;
    }

    void Renderer::updateScene(const SceneData& sceneData)
    {
        mCurrentScene = sceneData;
        // Filled in here rather than by the caller: the count belongs to the light buffer this frame,
        // which updateLights owns, and making callers keep the two in step would be a trap.
        mCurrentScene.lightCount = mLightCount;

        // A TLAS rebuild changes what casts shadows, which the accumulator's rejection tests cannot
        // detect -- a newly loaded building throwing a fresh shadow across ground whose depth and
        // normal are unchanged passes both tests and would keep its stale, now wrong, history.
        //
        // Zeroing the maximum history length for one frame forces the blend weight to 1 everywhere,
        // which discards the history without touching the images. The alternative is a
        // vkCmdClearColorImage on both, and a cell load already carries roughly 4,600 blocking
        // submits without adding another.
        if (mHistoryInvalid)
        {
            mCurrentScene.denoiseParams.w = 0.0f;
            mHistoryInvalid = false;
        }

        std::memcpy(mUniformMapped[mCurrentFrame], &mCurrentScene, sizeof(SceneData));
    }

    void Renderer::submitMesh(const MeshSubmission& submission)
    {
        // Only for meshes that will actually be drawn. The normal matrix is a 4x4 affine inverse and
        // transpose, its sole consumer is the G-buffer draw loop, and that loop skips anything
        // MeshSubmission::visible clears -- so computing it for culled meshes was work thrown away.
        //
        // It is worth roughly an order of magnitude here rather than a few percent: every instance in
        // every loaded cell is submitted every frame, ~9,300 on a Bitter Coast save, and frustum
        // culling leaves about 843 of them visible. The TLAS path is unaffected because it uses the
        // transform directly and deliberately ignores visibility -- an object behind the camera still
        // casts a shadow into view.
        // Only for meshes that will actually be drawn. The normal matrix is a 4x4 affine inverse and
        // transpose, its sole consumer is the G-buffer draw loop, and that loop skips anything
        // MeshSubmission::visible clears -- so computing it for culled meshes is work thrown away.
        // The TLAS path is unaffected: it uses the transform directly and deliberately ignores
        // visibility, because an object behind the camera still casts a shadow into view.
        //
        // **Measured, and it saves nothing.** With ~9,300 instances submitted per frame and frustum
        // culling leaving 500-800 of them visible, this skips the inverse for over 90% of them and
        // the submit loop stays at 6.0 ms/frame either way. Kept because it is free and removes
        // provably discarded work, not because it made anything faster. The real content of that
        // 6 ms is the per-instance iteration itself -- the transform copy, the frustum test, and
        // 9,300 push_backs of a 100-plus byte draw command -- so anyone optimising here should go
        // after the number of instances walked, not the arithmetic done per instance.
        Mat4 normalMatrix = {};
        if (submission.visible)
            normalMatrix = computeNormalMatrix(submission.transform);

        // An out-of-range slot would index past the end of the shader's sampler array, so anything the
        // caller could not fit into the array falls back to slot 0.
        const uint32_t slot = submission.textureIndex < maxSceneTextures ? submission.textureIndex : 0;
        mDrawCommands.push_back({ submission.vertexBuffer, submission.indexBuffer, submission.indexCount,
            submission.transform, normalMatrix, submission.blasAddress, submission.vertexAddress,
            submission.indexAddress, slot, submission.alphaTested, submission.roughness,
            submission.specularStrength, submission.visible, submission.skinBuffer,
            submission.boneOffset });
    }

    void Renderer::uploadGeometryTable(const std::vector<GeometryRecord>& records)
    {
        if (records.empty())
            return;

        const uint32_t needed = static_cast<uint32_t>(records.size());
        if (needed > mGeometryTableCapacity)
        {
            if (mGeometryTableMapped != nullptr)
            {
                vmaUnmapMemory(mDevice->allocator(), mGeometryTableMemory);
                mGeometryTableMapped = nullptr;
            }
            if (mGeometryTableBuffer != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->allocator(), mGeometryTableBuffer, mGeometryTableMemory);
                mGeometryTableBuffer = VK_NULL_HANDLE;
                mGeometryTableMemory = VK_NULL_HANDLE;
            }

            // Overshoot so a cell that adds a handful of instances does not force a reallocation.
            mGeometryTableCapacity = needed + needed / 2 + 64;
            createBufferLocal(*mDevice, sizeof(GeometryRecord) * mGeometryTableCapacity,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                mGeometryTableBuffer, mGeometryTableMemory);
            VK_CHECK(vmaMapMemory(mDevice->allocator(), mGeometryTableMemory, &mGeometryTableMapped));

            // The buffer handle changed, so every RT set has to be repointed at it.
            VkDescriptorBufferInfo bufferInfo = {};
            bufferInfo.buffer = mGeometryTableBuffer;
            bufferInfo.offset = 0;
            bufferInfo.range = VK_WHOLE_SIZE;

            for (VkDescriptorSet set : mRtDescriptorSets)
            {
                if (set == VK_NULL_HANDLE)
                    continue;
                VkWriteDescriptorSet write = {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = set;
                write.dstBinding = 6;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufferInfo;
                vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
            }
        }

        std::memcpy(mGeometryTableMapped, records.data(), sizeof(GeometryRecord) * records.size());
    }

    void Renderer::buildTlas()
    {
        mTlasDirty = false;

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        std::vector<GeometryRecord> records;
        instances.reserve(mDrawCommands.size());
        records.reserve(mDrawCommands.size());

        for (const MeshDrawCommand& drawCmd : mDrawCommands)
        {
            if (drawCmd.blasAddress == 0)
                continue;

            VkAccelerationStructureInstanceKHR instance = {};
            // VkTransformMatrixKHR is a row-major 3x4; Mat4 is column-major 4x4 with element
            // (row, col) at data[col * 4 + row].
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 4; ++col)
                    instance.transform.matrix[row][col] = drawCmd.transform.data[col * 4 + row];

            instance.mask = 0xFF;
            // Morrowind meshes are not reliably wound consistently, and backface-culled geometry would
            // drop shadow rays that pass through the back of a surface.
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference = drawCmd.blasAddress;
            // The hit shaders read records[gl_InstanceCustomIndexEXT], so the two arrays are filled in
            // lockstep here. Building the table anywhere else risks the indices drifting apart, which
            // would silently alpha-test against the wrong mesh's texture.
            instance.instanceCustomIndex = static_cast<uint32_t>(records.size());

            GeometryRecord record = {};
            record.vertexAddress = drawCmd.vertexAddress;
            record.indexAddress = drawCmd.indexAddress;
            record.textureIndex = drawCmd.textureIndex;
            record.alphaTested = drawCmd.alphaTested ? 1u : 0u;

            instances.push_back(instance);
            records.push_back(record);
        }

        // The old TLAS may still be referenced by in-flight frames.
        vkDeviceWaitIdle(mDevice->handle());
        mTlas.reset();

        if (instances.empty())
            return;

        uploadGeometryTable(records);

        mTlas = std::make_unique<AccelerationStructure>(
            AccelerationStructure::createTLAS(*mDevice, *mCommandPool, instances));

        VkAccelerationStructureKHR tlasHandle = mTlas->handle();

        VkWriteDescriptorSetAccelerationStructureKHR asInfo = {};
        asInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        asInfo.accelerationStructureCount = 1;
        asInfo.pAccelerationStructures = &tlasHandle;

        for (VkDescriptorSet set : mRtDescriptorSets)
        {
            VkWriteDescriptorSet write = {};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.pNext = &asInfo;
            write.dstSet = set;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;

            vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
        }

        Log(Debug::Info) << "Vulkan: TLAS built with " << instances.size() << " instances";
    }

    void Renderer::cleanup()
    {
        if (!mDevice)
            return;

        vkDeviceWaitIdle(mDevice->handle());

        VkDevice dev = mDevice->handle();

        mRtPipeline.reset();
        // Both own Vulkan handles and are declared after mDevice, so without an explicit reset here
        // their destructors would run *after* mDevice.reset() below and call vkDestroy* on a dead
        // VkDevice. mTlas is easy to miss because nothing else in the frame loop touches it.
        mTlas.reset();
        mFallbackTexture.reset();

        if (mGeometryTableMapped != nullptr)
        {
            vmaUnmapMemory(mDevice->allocator(), mGeometryTableMemory);
            mGeometryTableMapped = nullptr;
        }
        if (mGeometryTableBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mDevice->allocator(), mGeometryTableBuffer, mGeometryTableMemory);
            mGeometryTableBuffer = VK_NULL_HANDLE;
            mGeometryTableMemory = VK_NULL_HANDLE;
        }

        if (mRayTracingEnabled)
        {
            destroyRtOutput();
            destroyDenoiseTargets();
        }

        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
            if (mLightMapped[i])
            {
                vmaUnmapMemory(mDevice->allocator(), mLightMemory[i]);
                mLightMapped[i] = nullptr;
            }
            if (mLightBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->allocator(), mLightBuffers[i], mLightMemory[i]);
                mLightBuffers[i] = VK_NULL_HANDLE;
                mLightMemory[i] = VK_NULL_HANDLE;
            }
            if (mSkinMapped[i])
            {
                vmaUnmapMemory(mDevice->allocator(), mSkinMemory[i]);
                mSkinMapped[i] = nullptr;
            }
            if (mSkinBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->allocator(), mSkinBuffers[i], mSkinMemory[i]);
                mSkinBuffers[i] = VK_NULL_HANDLE;
                mSkinMemory[i] = VK_NULL_HANDLE;
            }
            if (mParticleMapped[i])
            {
                vmaUnmapMemory(mDevice->allocator(), mParticleMemory[i]);
                mParticleMapped[i] = nullptr;
            }
            if (mParticleBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->allocator(), mParticleBuffers[i], mParticleMemory[i]);
                mParticleBuffers[i] = VK_NULL_HANDLE;
                mParticleMemory[i] = VK_NULL_HANDLE;
            }
            if (mUniformMapped[i])
            {
                vmaUnmapMemory(mDevice->allocator(), mUniformMemory[i]);
                mUniformMapped[i] = nullptr;
            }
            if (mUniformBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mDevice->allocator(), mUniformBuffers[i], mUniformMemory[i]);
                mUniformBuffers[i] = VK_NULL_HANDLE;
                mUniformMemory[i] = VK_NULL_HANDLE;
            }
        }

        if (mDescriptorPool != VK_NULL_HANDLE)
            vkDestroyDescriptorPool(dev, mDescriptorPool, nullptr);
        if (mSceneDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, mSceneDescriptorLayout, nullptr);
        if (mCompositeDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, mCompositeDescriptorLayout, nullptr);
        if (mRtDescriptorLayout != VK_NULL_HANDLE)
            vkDestroyDescriptorSetLayout(dev, mRtDescriptorLayout, nullptr);

        if (mGBufferSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mGBufferSampler, nullptr);
        if (mTextureSampler != VK_NULL_HANDLE)
            vkDestroySampler(dev, mTextureSampler, nullptr);

        if (mParticlePipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mParticlePipeline, nullptr);
        if (mParticlePipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, mParticlePipelineLayout, nullptr);
        if (mGBufferSkinnedPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mGBufferSkinnedPipeline, nullptr);
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
