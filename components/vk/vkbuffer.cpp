#include "vkbuffer.hpp"

#include "vkcommon.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"

#include <cstring>
#include <utility>

namespace Vk
{
    Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
        : mDevice(device.handle())
        , mSize(size)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(mDevice, &bufferInfo, nullptr, &mBuffer));

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(mDevice, mBuffer, &memRequirements);

        VkMemoryAllocateFlagsInfo allocFlagsInfo{};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;

        bool needsDeviceAddress = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
        if (needsDeviceAddress)
            allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = device.findMemoryType(memRequirements.memoryTypeBits, properties);
        if (needsDeviceAddress)
            allocInfo.pNext = &allocFlagsInfo;

        VK_CHECK(vkAllocateMemory(mDevice, &allocInfo, nullptr, &mMemory));
        VK_CHECK(vkBindBufferMemory(mDevice, mBuffer, mMemory, 0));
    }

    Buffer::~Buffer()
    {
        cleanup();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : mDevice(other.mDevice)
        , mBuffer(other.mBuffer)
        , mMemory(other.mMemory)
        , mSize(other.mSize)
    {
        other.mBuffer = VK_NULL_HANDLE;
        other.mMemory = VK_NULL_HANDLE;
        other.mSize = 0;
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            mDevice = other.mDevice;
            mBuffer = other.mBuffer;
            mMemory = other.mMemory;
            mSize = other.mSize;
            other.mBuffer = VK_NULL_HANDLE;
            other.mMemory = VK_NULL_HANDLE;
            other.mSize = 0;
        }
        return *this;
    }

    void* Buffer::map()
    {
        void* data;
        VK_CHECK(vkMapMemory(mDevice, mMemory, 0, mSize, 0, &data));
        return data;
    }

    void Buffer::unmap()
    {
        vkUnmapMemory(mDevice, mMemory);
    }

    void Buffer::copyFrom(const void* data, VkDeviceSize size)
    {
        void* mapped = map();
        std::memcpy(mapped, data, static_cast<size_t>(size));
        unmap();
    }

    Buffer Buffer::createWithStaging(
        Device& device, CommandPool& commandPool, VkBufferUsageFlags usage, const void* data, VkDeviceSize size)
    {
        Buffer staging(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        staging.copyFrom(data, size);

        Buffer deviceBuffer(device, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        VkCommandBuffer cmd = commandPool.beginSingleTime();

        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(cmd, staging.handle(), deviceBuffer.handle(), 1, &copyRegion);

        commandPool.endSingleTime(cmd, device.graphicsQueue());

        return deviceBuffer;
    }

    VkDeviceAddress Buffer::deviceAddress() const
    {
        VkBufferDeviceAddressInfo addressInfo{};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = mBuffer;
        return vkGetBufferDeviceAddress(mDevice, &addressInfo);
    }

    void Buffer::cleanup()
    {
        if (mBuffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(mDevice, mBuffer, nullptr);
            mBuffer = VK_NULL_HANDLE;
        }
        if (mMemory != VK_NULL_HANDLE)
        {
            vkFreeMemory(mDevice, mMemory, nullptr);
            mMemory = VK_NULL_HANDLE;
        }
    }
}
