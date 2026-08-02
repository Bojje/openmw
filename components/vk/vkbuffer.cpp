#include "vkbuffer.hpp"

#include "vkcommon.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"

#include <cstring>
#include <utility>

#include <vk_mem_alloc.h>

namespace Vk
{
    Buffer::Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties)
        : mDevice(device.handle())
        , mAllocator(device.allocator())
        , mSize(size)
    {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // Suballocated out of VMA's pools rather than given its own vkAllocateMemory. A cell load
        // creates thousands of these; dedicated allocations made that thousands of driver allocations,
        // which is slow and wasteful once each is rounded up to the allocation granularity.
        //
        // The device address flag chaining that used to live here is gone: the allocator is created
        // with VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, so VMA adds
        // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT to the underlying block itself.
        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;

        // Callers still express intent as VkMemoryPropertyFlags, which is the right vocabulary at this
        // interface. Translate it: HOST_VISIBLE means the caller intends to map and write, which for
        // this renderer is always a staging upload or a persistently mapped uniform buffer.
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0)
        {
            allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            allocInfo.requiredFlags = properties;
        }
        else if (properties != 0)
        {
            allocInfo.requiredFlags = properties;
        }

        // create + allocate + bind in one call, so there is no window in which the VkBuffer exists but
        // the allocation has failed. The previous two-step version leaked the VkBuffer if the throw
        // landed between them -- which is exactly what happened when memory ran out.
        VK_CHECK(vmaCreateBuffer(mAllocator, &bufferInfo, &allocInfo, &mBuffer, &mAllocation, nullptr));
    }

    Buffer::~Buffer()
    {
        cleanup();
    }

    Buffer::Buffer(Buffer&& other) noexcept
        : mDevice(other.mDevice)
        , mAllocator(other.mAllocator)
        , mBuffer(other.mBuffer)
        , mAllocation(other.mAllocation)
        , mSize(other.mSize)
    {
        other.mBuffer = VK_NULL_HANDLE;
        other.mAllocation = VK_NULL_HANDLE;
        other.mSize = 0;
    }

    Buffer& Buffer::operator=(Buffer&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            mDevice = other.mDevice;
            mAllocator = other.mAllocator;
            mBuffer = other.mBuffer;
            mAllocation = other.mAllocation;
            mSize = other.mSize;
            other.mBuffer = VK_NULL_HANDLE;
            other.mAllocation = VK_NULL_HANDLE;
            other.mSize = 0;
        }
        return *this;
    }

    void* Buffer::map()
    {
        void* data;
        VK_CHECK(vmaMapMemory(mAllocator, mAllocation, &data));
        return data;
    }

    void Buffer::unmap()
    {
        vmaUnmapMemory(mAllocator, mAllocation);
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
            vmaDestroyBuffer(mAllocator, mBuffer, mAllocation);
            mBuffer = VK_NULL_HANDLE;
            mAllocation = VK_NULL_HANDLE;
        }
    }
}
