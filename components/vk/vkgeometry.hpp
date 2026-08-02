#ifndef OPENMW_COMPONENTS_VK_VKGEOMETRY_H
#define OPENMW_COMPONENTS_VK_VKGEOMETRY_H

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.h>

namespace Vk
{
    class AccelerationStructure;
    class Buffer;
    class CommandPool;
    class Device;

    // Interleaved vertex layout shared by everything the G-buffer pass draws, 48 bytes per vertex:
    //   position vec3  (offset 0)
    //   normal   vec3  (offset 12)
    //   texcoord vec2  (offset 24)
    //   colour   vec4  (offset 32)
    // Must stay in step with the attribute descriptions in Renderer::createGBufferPipeline and with
    // gbuffer.vert. Indices are always 32-bit; the draw loop hardcodes VK_INDEX_TYPE_UINT32.
    constexpr VkDeviceSize sVertexStride = 48;
    constexpr uint32_t sFloatsPerVertex = 12;

    // A drawable chunk of geometry on the device: vertex and index buffers plus, where the device
    // supports ray tracing, the BLAS built over them.
    struct Geometry
    {
        Geometry();
        ~Geometry();
        Geometry(Geometry&&) noexcept;
        Geometry& operator=(Geometry&&) noexcept;
        Geometry(const Geometry&) = delete;
        Geometry& operator=(const Geometry&) = delete;

        std::unique_ptr<Buffer> vertexBuffer;
        std::unique_ptr<Buffer> indexBuffer;
        std::unique_ptr<AccelerationStructure> blas;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;

        bool valid() const { return indexCount > 0; }
        VkDeviceAddress blasAddress() const;
    };

    // Uploads interleaved vertices (sFloatsPerVertex floats each) and 32-bit indices into device-local
    // buffers via staging, then builds a BLAS if the device supports ray tracing. Returns an empty
    // Geometry for degenerate input -- createBLAS computes maxVertex as vertexCount - 1, which
    // underflows to 0xFFFFFFFF when handed zero vertices.
    Geometry uploadGeometry(Device& device, CommandPool& commandPool, const float* vertexData,
        uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount);
}

#endif
