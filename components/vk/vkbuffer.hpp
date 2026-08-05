#ifndef OPENMW_COMPONENTS_VK_VKBUFFER_H
#define OPENMW_COMPONENTS_VK_VKBUFFER_H

#include <vulkan/vulkan.h>

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
        VkDeviceMemory memory() const { return mMemory; }
        VkDeviceSize size() const { return mSize; }
        VkDeviceAddress deviceAddress() const;

    private:
        void cleanup();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mMemory = VK_NULL_HANDLE;
        VkDeviceSize mSize = 0;
    };
}

#endif
