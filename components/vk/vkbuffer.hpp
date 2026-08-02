#ifndef OPENMW_COMPONENTS_VK_VKBUFFER_H
#define OPENMW_COMPONENTS_VK_VKBUFFER_H

#include <vulkan/vulkan.h>

// Forward declared so the 700 KB vk_mem_alloc.h header stays out of everything that holds a Buffer.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vk
{
    class Device;
    class CommandPool;

    class Buffer
    {
    public:
        Buffer() = default;
        Buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
        ~Buffer();

        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        Buffer(Buffer&& other) noexcept;
        Buffer& operator=(Buffer&& other) noexcept;

        void* map();
        void unmap();
        void copyFrom(const void* data, VkDeviceSize size);

        static Buffer createWithStaging(Device& device, CommandPool& commandPool, VkBufferUsageFlags usage,
            const void* data, VkDeviceSize size);

        VkBuffer handle() const { return mBuffer; }
        VkDeviceSize size() const { return mSize; }
        VkDeviceAddress deviceAddress() const;

    private:
        void cleanup();

        VkDevice mDevice = VK_NULL_HANDLE;
        VmaAllocator mAllocator = VK_NULL_HANDLE;
        VkBuffer mBuffer = VK_NULL_HANDLE;
        // Suballocation out of a VMA pool, not a dedicated VkDeviceMemory. There is deliberately no
        // memory() accessor any more: the underlying VkDeviceMemory is shared with other buffers, so
        // handing it out would invite someone to map or free the whole block.
        VmaAllocation mAllocation = VK_NULL_HANDLE;
        VkDeviceSize mSize = 0;
    };
}

#endif
