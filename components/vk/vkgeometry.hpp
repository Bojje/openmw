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

    // Skinned geometry carries a second vertex buffer alongside that one, eight bytes per vertex:
    //   bone indices  u8vec4  (offset 0)
    //   bone weights  unorm8x4 (offset 4)
    // Produced by NifVk::MeshConverter, bound as binding 1 by the skinned G-buffer pipeline, and read
    // by gbuffer_skinned.vert. The indices are into the shape's own bone list, offset by the
    // instance's boneOffset push constant.
    constexpr VkDeviceSize sSkinStride = 8;

    // Bone matrices the skinned pipeline can hold in one frame, across every actor on screen. One
    // matrix is 64 bytes, so this is a megabyte per frame in flight.
    //
    // Sized against what a busy Morrowind exterior actually submits: a few hundred actor shapes, each
    // naming the handful of bones its own vertices are weighted to rather than the whole skeleton.
    // Overflowing is not fatal -- the shapes that do not fit are drawn in bind pose -- but it is
    // reported once, because the symptom on its own reads as a broken skeleton.
    constexpr uint32_t maxSkinMatrices = 16384;

    /// A bone offset meaning "this shape is not skinned this frame".
    constexpr uint32_t sNoBones = 0xFFFFFFFFu;

    // One entry per TLAS instance, indexed in the hit shaders by gl_InstanceCustomIndexEXT. This is
    // what lets an any-hit shader alpha-test a leaf billboard: without it the hit shaders have no way
    // to reach the geometry they hit, so every alpha-cutout quad occludes as a solid rectangle.
    //
    // The layout is mirrored by the GeometryRecord struct declared in anyhit.rahit and closesthit.rchit.
    // Both addresses are declared there as buffer_reference types, which are 8-byte aligned, so this
    // packs identically under std430. Keep the padding: it pins the stride at 32 bytes.
    struct GeometryRecord
    {
        VkDeviceAddress vertexAddress = 0;
        VkDeviceAddress indexAddress = 0;
        uint32_t textureIndex = 0;
        uint32_t alphaTested = 0;
        uint32_t pad0 = 0;
        uint32_t pad1 = 0;
    };

    static_assert(sizeof(GeometryRecord) == 32, "GeometryRecord must match the std430 layout in the hit shaders");
    static_assert(offsetof(GeometryRecord, indexAddress) == 8, "GeometryRecord layout drifted from the shaders");
    static_assert(offsetof(GeometryRecord, textureIndex) == 16, "GeometryRecord layout drifted from the shaders");
    static_assert(offsetof(GeometryRecord, alphaTested) == 20, "GeometryRecord layout drifted from the shaders");

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
        VkDeviceAddress vertexAddress() const;
        VkDeviceAddress indexAddress() const;
    };

    // Uploads interleaved vertices (sFloatsPerVertex floats each) and 32-bit indices into device-local
    // buffers via staging, then builds a BLAS if the device supports ray tracing. Returns an empty
    // Geometry for degenerate input -- createBLAS computes maxVertex as vertexCount - 1, which
    // underflows to 0xFFFFFFFF when handed zero vertices.
    //
    // \a alphaTested must be true for anything whose silhouette lives in its texture's alpha channel.
    // Such geometry is left non-opaque in the BLAS so the any-hit shader runs and can discard the
    // transparent texels; flagging it opaque makes a leaf billboard cast a solid rectangular shadow.
    Geometry uploadGeometry(Device& device, CommandPool& commandPool, const float* vertexData,
        uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, bool alphaTested);
}

#endif
