#ifndef OPENMW_COMPONENTS_VK_VKBUFFER_H
#define OPENMW_COMPONENTS_VK_VKBUFFER_H

#include <vulkan/vulkan.h>

// Forward declared so the 700 KB vk_mem_alloc.h header stays out of everything that holds a Buffer.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vk
{
    class Device;
    class CommandBatch;
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

        // Self-contained form: allocates a staging buffer, copies through it and blocks until the GPU
        // has finished, so the staging buffer is gone by the time this returns.
        static Buffer createWithStaging(Device& device, CommandPool& commandPool, VkBufferUsageFlags usage,
            const void* data, VkDeviceSize size);

        // Batched form: records the staging copy into \a batch instead of submitting on its own, so a
        // caller filling several buffers pays for one submit rather than one per buffer.
        //
        // The copy has NOT happened when this returns. The GPU only reads the staging buffer when the
        // batch is submitted, so \a stagingOut receives it and the caller must keep it alive until
        // batch.flush() -- or the batch's destructor -- has returned. Destroying it earlier is a
        // use-after-free that shows up as randomly corrupted buffer contents. The staging buffer is an
        // out parameter rather than an internal detail precisely so that obligation cannot be missed.
        static Buffer createWithStaging(Device& device, CommandBatch& batch, VkBufferUsageFlags usage,
            const void* data, VkDeviceSize size, Buffer& stagingOut);

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
