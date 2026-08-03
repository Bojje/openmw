#include "vktexture.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

#include <vk_mem_alloc.h>

#include "vkbuffer.hpp"
#include "vkcommands.hpp"
#include "vkcommon.hpp"
#include "vkdevice.hpp"

namespace
{
    // Number of mip levels a width x height image can have, counting level 0. Each level halves until
    // both dimensions reach 1, so this is floor(log2(max(w, h))) + 1. Asking for more than this is
    // invalid image usage, not merely wasteful, so a source claiming more levels gets clamped.
    uint32_t maxMipLevels(uint32_t width, uint32_t height)
    {
        uint32_t extent = std::max(width, height);
        uint32_t levels = 1;
        while (extent > 1)
        {
            extent /= 2;
            ++levels;
        }
        return levels;
    }
}

namespace Vk
{
    Texture::~Texture()
    {
        destroy();
    }

    Texture::Texture(Texture&& other) noexcept
        : mDevice(other.mDevice)
        , mAllocator(other.mAllocator)
        , mImage(other.mImage)
        , mAllocation(other.mAllocation)
        , mView(other.mView)
        , mMipLevels(other.mMipLevels)
        , mWidth(other.mWidth)
        , mHeight(other.mHeight)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mAllocator = VK_NULL_HANDLE;
        other.mImage = VK_NULL_HANDLE;
        other.mAllocation = VK_NULL_HANDLE;
        other.mView = VK_NULL_HANDLE;
        other.mMipLevels = 1;
        other.mWidth = 0;
        other.mHeight = 0;
    }

    Texture& Texture::operator=(Texture&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = std::exchange(other.mDevice, VK_NULL_HANDLE);
            mAllocator = std::exchange(other.mAllocator, VK_NULL_HANDLE);
            mImage = std::exchange(other.mImage, VK_NULL_HANDLE);
            mAllocation = std::exchange(other.mAllocation, VK_NULL_HANDLE);
            mView = std::exchange(other.mView, VK_NULL_HANDLE);
            mMipLevels = std::exchange(other.mMipLevels, 1);
            mWidth = std::exchange(other.mWidth, 0);
            mHeight = std::exchange(other.mHeight, 0);
        }
        return *this;
    }

    void Texture::destroy()
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        if (mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice, mView, nullptr);
        // Destroys the image and returns its suballocation to the pool in one call; the memory is owned
        // by the allocator, so there is nothing left to free afterwards.
        if (mImage != VK_NULL_HANDLE)
            vmaDestroyImage(mAllocator, mImage, mAllocation);

        mView = VK_NULL_HANDLE;
        mImage = VK_NULL_HANDLE;
        mAllocation = VK_NULL_HANDLE;
        mAllocator = VK_NULL_HANDLE;
        mDevice = VK_NULL_HANDLE;
    }

    VkDeviceSize Texture::levelSizeInBytes(VkFormat format, uint32_t width, uint32_t height)
    {
        // Rounded up: the last levels of a chain are smaller than a block in one or both dimensions
        // and still cost a whole block. A 1x1 BC1 level is 8 bytes, not 8/16th of one.
        const VkDeviceSize blocksWide = (width + 3) / 4;
        const VkDeviceSize blocksHigh = (height + 3) / 4;

        switch (format)
        {
            // BC1 packs a 4x4 block into 8 bytes; BC2/BC3 add an alpha block, so 16. The UNORM and
            // SRGB variants of a format differ only in how samples are interpreted, never in size.
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                return blocksWide * blocksHigh * 8;
            case VK_FORMAT_BC2_UNORM_BLOCK:
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
                return blocksWide * blocksHigh * 16;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return static_cast<VkDeviceSize>(width) * height * 4;
            // Single channel, for masks rather than colour -- the land texture blend maps are the
            // first. Deliberately UNORM only: a weight is not a colour and must not be sRGB decoded
            // on sample, or the ramp between two land textures acquires a curve.
            case VK_FORMAT_R8_UNORM:
                return static_cast<VkDeviceSize>(width) * height;
            default:
                return 0;
        }
    }

    VkDeviceSize Texture::mipChainSizeInBytes(
        VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels)
    {
        const uint32_t levels = std::min(std::max(mipLevels, 1u), maxMipLevels(width, height));

        VkDeviceSize total = 0;
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;

        for (uint32_t level = 0; level < levels; ++level)
        {
            const VkDeviceSize size = levelSizeInBytes(format, levelWidth, levelHeight);
            if (size == 0)
                return 0;

            total += size;
            levelWidth = std::max(levelWidth / 2, 1u);
            levelHeight = std::max(levelHeight / 2, 1u);
        }

        return total;
    }

    Texture Texture::create(Device& device, CommandPool& commandPool, uint32_t width, uint32_t height,
        VkFormat format, const void* data, VkDeviceSize dataSize, uint32_t mipLevels)
    {
        // One copy region per level. The offsets are derived here rather than taken from the caller so
        // that the block arithmetic lives in exactly one place; a level's size is never inferred from
        // its pixel count, which is the mistake that makes a compressed upload read off the end of the
        // decoded image.
        std::vector<VkBufferImageCopy> regions;

        const uint32_t requestedLevels = std::min(std::max(mipLevels, 1u), maxMipLevels(width, height));
        regions.reserve(requestedLevels);

        VkDeviceSize levelOffset = 0;
        uint32_t levelWidth = width;
        uint32_t levelHeight = height;

        for (uint32_t level = 0; level < requestedLevels; ++level)
        {
            // A single-level upload trusts dataSize verbatim, exactly as this function always has:
            // callers pass sizes for formats levelSizeInBytes does not know about, and the fallback
            // 1x1 white texture is one of them.
            const VkDeviceSize levelSize
                = requestedLevels == 1 ? dataSize : levelSizeInBytes(format, levelWidth, levelHeight);

            // Stop rather than guess. An unknown format sizes to 0, and a chain the source is too
            // small for would have this level running past the end of data.
            if (levelSize == 0 || levelOffset + levelSize > dataSize)
                break;

            VkBufferImageCopy region = {};
            region.bufferOffset = levelOffset;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = { levelWidth, levelHeight, 1 };
            regions.push_back(region);

            levelOffset += levelSize;
            levelWidth = std::max(levelWidth / 2, 1u);
            levelHeight = std::max(levelHeight / 2, 1u);
        }

        if (regions.empty())
            throw std::runtime_error("Vulkan: texture upload has no usable mip level");

        // The image gets only the levels that were actually filled, so there is no level whose
        // contents are undefined for the sampler to select between.
        const uint32_t levelCount = static_cast<uint32_t>(regions.size());
        // Only the levels being uploaded need staging; a truncated chain does not stage its tail.
        const VkDeviceSize stagingSize = levelOffset;

        Texture texture;
        texture.mDevice = device.handle();
        texture.mAllocator = device.allocator();

        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = levelCount;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        // AUTO picks device-local for an image that is only ever sampled and transfer-copied into.
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        // Create, allocate and bind happen in one call, so there is no longer a window where a failed
        // allocation leaves a created-but-unbound image behind to be cleaned up.
        VK_CHECK(vmaCreateImage(
            texture.mAllocator, &imageInfo, &allocInfo, &texture.mImage, &texture.mAllocation, nullptr));

        // Staging upload. The source data is used verbatim, so block-compressed formats are copied
        // as blocks without any decode step. The whole chain is one contiguous blob, so it stages in a
        // single copy and the per-level regions just index into it.
        Buffer staging(device, stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.copyFrom(data, stagingSize);

        VkCommandBuffer cmd = commandPool.beginSingleTime();

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.mImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        // Every level, not just level 0. A barrier only covers the subresources it names, so leaving
        // this at 1 would copy into levels that are still in UNDEFINED layout and hand the sampler
        // levels that were never transitioned to SHADER_READ_ONLY_OPTIMAL.
        barrier.subresourceRange.levelCount = levelCount;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // All levels in one submission; they are independent regions of the same image and nothing
        // reads a level before the layout transition below.
        vkCmdCopyBufferToImage(cmd, staging.handle(), texture.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            levelCount, regions.data());

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        // The ray tracing stages matter as much as the fragment stage here: the any-hit and closest-hit
        // shaders sample these same textures through the RT descriptor set. Naming only the fragment
        // stage is harmless today because endSingleTime drains the queue immediately afterwards, but it
        // becomes a real missing dependency the moment uploads stop being synchronous.
        VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        if (device.rayTracingSupported())
            dstStage |= VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, dstStage,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        commandPool.endSingleTime(cmd, device.graphicsQueue());

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture.mImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        // The view has to span the whole chain or the sampler has nothing to select between, however
        // many levels the image itself was created with.
        viewInfo.subresourceRange.levelCount = levelCount;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(texture.mDevice, &viewInfo, nullptr, &texture.mView));

        texture.mMipLevels = levelCount;
        texture.mWidth = width;
        texture.mHeight = height;

        return texture;
    }

    Texture Texture::createRenderTarget(
        Device& device, uint32_t width, uint32_t height, VkFormat format, uint32_t mipLevels)
    {
        if (width == 0 || height == 0)
            throw std::runtime_error("render target has a zero dimension");
        if (levelSizeInBytes(format, 4, 4) == 0)
            throw std::runtime_error("render target format is not one this class can size");

        // Clamped to what the dimensions actually admit, so the chain always ends at 1x1 and never
        // names a level the image does not have.
        uint32_t maxLevels = 1;
        for (uint32_t w = width, h = height; w > 1 || h > 1; ++maxLevels)
        {
            w = w > 1 ? w / 2 : 1;
            h = h > 1 ? h / 2 : 1;
        }
        const uint32_t levelCount = std::max(1u, std::min(mipLevels, maxLevels));

        Texture texture;
        texture.mDevice = device.handle();
        texture.mAllocator = device.allocator();
        texture.mMipLevels = levelCount;
        texture.mWidth = width;
        texture.mHeight = height;

        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = levelCount;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        // TRANSFER_SRC as well as DST: generateMipChain blits level n-1 into level n, so every level
        // is both a source and a destination at some point.
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        VK_CHECK(vmaCreateImage(
            texture.mAllocator, &imageInfo, &allocInfo, &texture.mImage, &texture.mAllocation, nullptr));

        VkImageViewCreateInfo viewInfo = {};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = texture.mImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = levelCount;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(texture.mDevice, &viewInfo, nullptr, &texture.mView));

        return texture;
    }

    void Texture::generateMipChain(Device& device, CommandPool& commandPool)
    {
        if (mImage == VK_NULL_HANDLE || mMipLevels <= 1)
            return;

        VkCommandBuffer cmd = commandPool.beginSingleTime();

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = mImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;

        int32_t width = static_cast<int32_t>(mWidth);
        int32_t height = static_cast<int32_t>(mHeight);

        for (uint32_t level = 1; level < mMipLevels; ++level)
        {
            // The source is whichever level was written last -- level 0 by the render pass, every one
            // after that by the previous blit -- so it moves from SHADER_READ_ONLY to TRANSFER_SRC.
            barrier.subresourceRange.baseMipLevel = level - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            barrier.subresourceRange.baseMipLevel = level;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            const int32_t nextWidth = width > 1 ? width / 2 : 1;
            const int32_t nextHeight = height > 1 ? height / 2 : 1;

            VkImageBlit blit = {};
            blit.srcOffsets[1] = { width, height, 1 };
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = level - 1;
            blit.srcSubresource.layerCount = 1;
            blit.dstOffsets[1] = { nextWidth, nextHeight, 1 };
            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = level;
            blit.dstSubresource.layerCount = 1;

            vkCmdBlitImage(cmd, mImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

            // The source level is finished with; put it back where a sampler expects it.
            barrier.subresourceRange.baseMipLevel = level - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            // The level just written becomes the next iteration's source, so it has to be in
            // SHADER_READ_ONLY when the loop comes round -- which is also where the last level has to
            // be left once the loop ends.
            barrier.subresourceRange.baseMipLevel = level;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            width = nextWidth;
            height = nextHeight;
        }

        commandPool.endSingleTime(cmd, device.graphicsQueue());
    }
}
