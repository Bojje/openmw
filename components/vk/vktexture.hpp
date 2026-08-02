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
    // Mip levels are never generated here. Morrowind's DDS files already ship a full chain, and
    // vkCmdBlitImage -- the usual way to generate one -- cannot operate on block-compressed formats
    // anyway. The chain is uploaded straight from the source data.
    class Texture
    {
    public:
        Texture() = default;
        ~Texture();

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;
        Texture(Texture&& other) noexcept;
        Texture& operator=(Texture&& other) noexcept;

        // Byte size of a single mip level of \a format at the given dimensions.
        //
        // This is the only place the layout of a level is decided, because getting it wrong is a read
        // past the end of the source buffer rather than a visible glitch. Block-compressed levels are
        // measured in whole 4x4 blocks, so the block counts are rounded up: a 2x2 BC1 level still
        // occupies one full 8-byte block. Returns 0 for formats whose layout is not known here, which
        // callers must treat as "cannot size this", not as "empty".
        static VkDeviceSize levelSizeInBytes(VkFormat format, uint32_t width, uint32_t height);

        // Byte size of \a mipLevels consecutive levels starting at width x height, i.e. the size of the
        // blob create() expects. Returns 0 if the format is not known.
        static VkDeviceSize mipChainSizeInBytes(
            VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels);

        // Uploads a mip chain of the given Vulkan format.
        //
        // \a data points at level 0 of \a mipLevels levels laid out back to back with no padding
        // between them, each level halving in both dimensions down to a minimum of 1. That is the
        // layout of a DDS file and the layout osg::Image keeps its mipmap data in, so a decoded image
        // can be handed over as one blob. \a dataSize is the byte size of the whole chain (see
        // mipChainSizeInBytes); for block-compressed formats that is blocks, not pixels.
        //
        // mipLevels defaults to 1, which uploads \a data as a lone level 0 and is the behaviour every
        // caller had before the chain was honoured. Levels that \a dataSize does not cover, or that the
        // dimensions do not admit, are dropped: the image is created with only the levels that were
        // actually uploaded, so no level is ever left undefined. Throws on failure.
        static Texture create(Device& device, CommandPool& commandPool, uint32_t width, uint32_t height,
            VkFormat format, const void* data, VkDeviceSize dataSize, uint32_t mipLevels = 1);

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
