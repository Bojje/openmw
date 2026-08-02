#include "vkrenderer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <vk_mem_alloc.h>

#include <components/debug/debuglog.hpp>

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
        createDescriptorSets();
        createGBufferPipeline();
        createCompositePipeline();

        if (mDevice->rayTracingSupported())
        {
            loadRayTracingFunctions(mDevice->handle());
            createRtOutput();
            createRtDescriptorSets();
            mRayTracingEnabled = true;

            writeCompositeDescriptor(3, mRtOutput.view);
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
        // Only mip level 0 is uploaded by Vk::Texture, so there is no chain to select between.
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        // Morrowind's UVs routinely run outside [0, 1] and rely on wrapping.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

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

    // Descriptor set layouts

    void Renderer::createDescriptorSetLayouts()
    {
        // Scene layout (set 0 for G-buffer pass): camera UBO + the scene texture array
        {
            std::array<VkDescriptorSetLayoutBinding, 2> bindings = {};

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

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mSceneDescriptorLayout));
        }

        // Composite layout: G-buffer textures + RT output + scene UBO
        {
            std::array<VkDescriptorSetLayoutBinding, 6> bindings = {};

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

            VkDescriptorSetLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
            layoutInfo.pBindings = bindings.data();

            VK_CHECK(vkCreateDescriptorSetLayout(mDevice->handle(), &layoutInfo, nullptr, &mCompositeDescriptorLayout));
        }

        // RT layout: TLAS + storage image + G-buffer samplers
        if (mDevice->rayTracingSupported())
        {
            std::array<VkDescriptorSetLayoutBinding, 8> bindings = {};

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
                16 + maxFramesInFlight * 4 + maxSceneTextures * maxFramesInFlight },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, maxFramesInFlight + 2 },
        };

        uint32_t maxSets = maxFramesInFlight * 2 + 4;

        if (mDevice->rayTracingSupported())
        {
            poolSizes.push_back({ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, maxFramesInFlight });
            // The RT sets carry a second copy of the sampler array (binding 7) for the hit shaders,
            // plus the geometry table.
            poolSizes.push_back({ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, maxFramesInFlight });
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

                VkWriteDescriptorSet write = {};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = mSceneDescriptorSets[i];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.pBufferInfo = &bufferInfo;

                vkUpdateDescriptorSets(mDevice->handle(), 1, &write, 0, nullptr);
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

            // Bind the scene UBO to each per-frame composite descriptor set
            for (uint32_t i = 0; i < maxFramesInFlight; i++)
            {
                VkDescriptorBufferInfo bufferInfo = {};
                bufferInfo.buffer = mUniformBuffers[i];
                bufferInfo.offset = 0;
                bufferInfo.range = sizeof(SceneData);

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
        mCommandPool->endSingleTime(cmd, mDevice->graphicsQueue());
    }

    void Renderer::destroyRtOutput()
    {
        VkDevice dev = mDevice->handle();
        if (mRtOutput.view != VK_NULL_HANDLE) { vkDestroyImageView(dev, mRtOutput.view, nullptr); mRtOutput.view = VK_NULL_HANDLE; }
        if (mRtOutput.image != VK_NULL_HANDLE)
        {
            vmaDestroyImage(mDevice->allocator(), mRtOutput.image, mRtOutput.memory);
            mRtOutput.image = VK_NULL_HANDLE;
            mRtOutput.memory = VK_NULL_HANDLE;
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

        for (uint32_t frame = 0; frame < maxFramesInFlight; ++frame)
        {
            VkDescriptorBufferInfo sceneInfo = {};
            sceneInfo.buffer = mUniformBuffers[frame];
            sceneInfo.offset = 0;
            sceneInfo.range = sizeof(SceneData);

            std::array<VkWriteDescriptorSet, 5> writes = {};

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
        pushConstant.size = sizeof(Vec4) * 3;
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

                for (const auto& drawCmd : mDrawCommands)
                {
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

                    vkCmdPushConstants(cmd, mGBufferPipelineLayout,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(pushData), &pushData);

                    VkDeviceSize offset = 0;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &drawCmd.vertexBuffer, &offset);
                    vkCmdBindIndexBuffer(cmd, drawCmd.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, drawCmd.indexCount, 1, 0, 0, 0);
                }
            }

            vkCmdEndRenderPass(cmd);
        }

        // 2. Ray tracing pass (shadows + reflections)
        if (mRayTracingEnabled && mRtPipeline && mTlas)
        {
            transitionImageLayout(cmd, mRtOutput.image,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_ASPECT_COLOR_BIT);

            mRtPipeline->bind(cmd);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR,
                mRtPipeline->layout(), 0, 1, &mRtDescriptorSets[mCurrentFrame], 0, nullptr);

            mRtPipeline->traceRays(cmd, extent.width, extent.height);

            transitionImageLayout(cmd, mRtOutput.image,
                VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
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

                std::array<Vec4, 3> compositePush = { sceneData->sunDirection, sceneData->sunColor, cameraPos };
                vkCmdPushConstants(cmd, mCompositePipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                    0, sizeof(Vec4) * 3, compositePush.data());

                vkCmdDraw(cmd, 3, 1, 0, 0);
            }

            vkCmdEndRenderPass(cmd);
        }

        mDrawCommands.clear();

        endFrame();
    }

    void Renderer::resize(uint32_t width, uint32_t height)
    {
        vkDeviceWaitIdle(mDevice->handle());

        if (mGBufferFramebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(mDevice->handle(), mGBufferFramebuffer, nullptr);
            mGBufferFramebuffer = VK_NULL_HANDLE;
        }
        destroyCompositeFramebuffers();
        destroyGBuffer();

        if (mRayTracingEnabled)
            destroyRtOutput();

        mSwapchain->recreate(width, height);
        mFrameSync->resizeImageSemaphores(static_cast<uint32_t>(mSwapchain->imageViews().size()));

        createGBuffer();
        createGBufferFramebuffer();
        createCompositeFramebuffers();

        if (mRayTracingEnabled)
        {
            createRtOutput();
            createRtDescriptorSets();
        }

        writeCompositeDescriptor(0, mGBuffer.albedoView);
        writeCompositeDescriptor(1, mGBuffer.normalView);
        writeCompositeDescriptor(2, mGBuffer.depthView);
        writeCompositeDescriptor(5, mGBuffer.materialView);

        if (mRtOutput.view != VK_NULL_HANDLE)
            writeCompositeDescriptor(3, mRtOutput.view);
        else
            writeCompositeDescriptor(3, mGBuffer.albedoView);
    }

    void Renderer::waitIdle()
    {
        vkDeviceWaitIdle(mDevice->handle());
    }

    void Renderer::updateScene(const SceneData& sceneData)
    {
        mCurrentScene = sceneData;
        std::memcpy(mUniformMapped[mCurrentFrame], &sceneData, sizeof(SceneData));
    }

    void Renderer::submitMesh(const MeshSubmission& submission)
    {
        Mat4 normalMatrix = computeNormalMatrix(submission.transform);
        // An out-of-range slot would index past the end of the shader's sampler array, so anything the
        // caller could not fit into the array falls back to slot 0.
        const uint32_t slot = submission.textureIndex < maxSceneTextures ? submission.textureIndex : 0;
        mDrawCommands.push_back({ submission.vertexBuffer, submission.indexBuffer, submission.indexCount,
            submission.transform, normalMatrix, submission.blasAddress, submission.vertexAddress,
            submission.indexAddress, slot, submission.alphaTested, submission.roughness,
            submission.specularStrength });
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
            destroyRtOutput();

        for (uint32_t i = 0; i < maxFramesInFlight; i++)
        {
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
