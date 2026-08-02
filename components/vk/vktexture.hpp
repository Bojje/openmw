#ifndef OPENMW_COMPONENTS_VK_VKTEXTURE_H
#define OPENMW_COMPONENTS_VK_VKTEXTURE_H

#include <cstdint>

#include <vulkan/vulkan.h>

// Forward declared rather than including vk_mem_alloc.h, which is a 700 KB single-header library and
// would land in every translation unit that touches a Texture.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vk
{
    class Device;
    class CommandPool;

    // A sampled 2D image uploaded from CPU pixel data.
    //
    // Morrowind's textures are predominantly DDS/S3TC. Vulkan supports the BC1/BC2/BC3 block formats
    // natively, so compressed data is uploaded as-is rather than being decoded first: less VRAM, less
    // code, and no decompression pass. Uncompressed sources are expected to arrive as RGBA8.
    //
    // Only mip level 0 is uploaded for now; mip generation is a separate task.
    class Texture
    {
    public:
        Texture() = default;
        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        // Uploads pixel data of the given Vulkan format. dataSize must be the exact byte size of the
        // level (for block-compressed formats that is blocks, not pixels). Throws on failure.
        static Texture create(Device& device, CommandPool& commandPool, uint32_t width, uint32_t height,
            VkFormat format, const void* data, VkDeviceSize dataSize);

        bool valid() const { return mImage != VK_NULL_HANDLE; }
        VkImage image() const { return mImage; }
        VkImageView view() const { return mView; }

    private:
        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        // Kept alongside the allocation because teardown needs the allocator and Texture holds no Device&.
        VmaAllocator mAllocator = VK_NULL_HANDLE;
        VkImage mImage = VK_NULL_HANDLE;
        VmaAllocation mAllocation = VK_NULL_HANDLE;
        VkImageView mView = VK_NULL_HANDLE;
    };
}

#endif
