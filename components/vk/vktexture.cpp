#include "vktexture.hpp"

#include <utility>

#include "vkbuffer.hpp"
#include "vkcommands.hpp"
#include "vkcommon.hpp"
#include "vkdevice.hpp"

namespace Vk
{
    Texture::~Texture()
    {
        destroy();
    }

    Texture::Texture(Texture&& other) noexcept
        : mDevice(other.mDevice)
        , mImage(other.mImage)
        , mMemory(other.mMemory)
        , mView(other.mView)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mImage = VK_NULL_HANDLE;
        other.mMemory = VK_NULL_HANDLE;
        other.mView = VK_NULL_HANDLE;
    }

    Texture& Texture::operator=(Texture&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = std::exchange(other.mDevice, VK_NULL_HANDLE);
            mImage = std::exchange(other.mImage, VK_NULL_HANDLE);
            mMemory = std::exchange(other.mMemory, VK_NULL_HANDLE);
            mView = std::exchange(other.mView, VK_NULL_HANDLE);
        }
        return *this;
    }

    void Texture::destroy()
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        if (mView != VK_NULL_HANDLE)
            vkDestroyImageView(mDevice, mView, nullptr);
        if (mImage != VK_NULL_HANDLE)
            vkDestroyImage(mDevice, mImage, nullptr);
        if (mMemory != VK_NULL_HANDLE)
            vkFreeMemory(mDevice, mMemory, nullptr);

        mView = VK_NULL_HANDLE;
        mImage = VK_NULL_HANDLE;
        mMemory = VK_NULL_HANDLE;
        mDevice = VK_NULL_HANDLE;
    }

    Texture Texture::create(Device& device, CommandPool& commandPool, uint32_t width, uint32_t height,
        VkFormat format, const void* data, VkDeviceSize dataSize)
    {
        Texture texture;
        texture.mDevice = device.handle();

        VkImageCreateInfo imageInfo = {};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = { width, height, 1 };
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VK_CHECK(vkCreateImage(texture.mDevice, &imageInfo, nullptr, &texture.mImage));

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(texture.mDevice, texture.mImage, &memRequirements);

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex
            = device.findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VK_CHECK(vkAllocateMemory(texture.mDevice, &allocInfo, nullptr, &texture.mMemory));
        VK_CHECK(vkBindImageMemory(texture.mDevice, texture.mImage, texture.mMemory, 0));

        // Staging upload. The source data is used verbatim, so block-compressed formats are copied
        // as blocks without any decode step.
        Buffer staging(device, dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.copyFrom(data, dataSize);

        VkCommandBuffer cmd = commandPool.beginSingleTime();

        VkImageMemoryBarrier barrier = {};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = texture.mImage;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = { width, height, 1 };

        vkCmdCopyBufferToImage(
            cmd, staging.handle(), texture.mImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

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
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;

        VK_CHECK(vkCreateImageView(texture.mDevice, &viewInfo, nullptr, &texture.mView));

        return texture;
    }
}
