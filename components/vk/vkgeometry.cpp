#include "vkgeometry.hpp"

#include "vkbuffer.hpp"
#include "vkcommands.hpp"
#include "vkdevice.hpp"
#include "vkraytracing.hpp"

namespace Vk
{
    // Out of line so the header can forward declare Buffer and AccelerationStructure rather than
    // pulling vkbuffer.hpp and vkraytracing.hpp into everything that holds a Geometry.
    Geometry::Geometry() = default;
    Geometry::~Geometry() = default;
    Geometry::Geometry(Geometry&&) noexcept = default;
    Geometry& Geometry::operator=(Geometry&&) noexcept = default;

    VkDeviceAddress Geometry::blasAddress() const
    {
        return blas ? blas->deviceAddress() : VkDeviceAddress{ 0 };
    }

    VkDeviceAddress Geometry::vertexAddress() const
    {
        return vertexBuffer ? vertexBuffer->deviceAddress() : VkDeviceAddress{ 0 };
    }

    VkDeviceAddress Geometry::indexAddress() const
    {
        return indexBuffer ? indexBuffer->deviceAddress() : VkDeviceAddress{ 0 };
    }

    Geometry uploadGeometry(Device& device, CommandPool& commandPool, const float* vertexData,
        uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount, bool alphaTested)
    {
        Geometry geometry;

        if (vertexData == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0)
            return geometry;

        VkBufferUsageFlags vertexFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBufferUsageFlags indexFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (device.rayTracingSupported())
        {
            vertexFlags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            indexFlags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }

        const VkDeviceSize vertexSize = static_cast<VkDeviceSize>(vertexCount) * sVertexStride;
        const VkDeviceSize indexSize = static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t);

        // Both staging copies go into one batch and one submit; separately they were two submits and
        // two full pipeline stalls per mesh, and a cell load uploads thousands of meshes.
        //
        // Declaration order is the lifetime rule, not tidiness. The GPU reads these staging buffers when
        // the batch is submitted, not when the copy is recorded, so they are declared ahead of the batch
        // and therefore destroyed after it -- including while unwinding, where the batch destructor
        // submits and blocks before either staging buffer is freed.
        Buffer vertexStaging;
        Buffer indexStaging;
        CommandBatch batch(commandPool, device.graphicsQueue());

        auto vb = Buffer::createWithStaging(device, batch, vertexFlags, vertexData, vertexSize, vertexStaging);
        geometry.vertexBuffer = std::make_unique<Buffer>(std::move(vb));

        auto ib = Buffer::createWithStaging(device, batch, indexFlags, indices, indexSize, indexStaging);
        geometry.indexBuffer = std::make_unique<Buffer>(std::move(ib));

        // No barrier is needed between the two copies: they write disjoint buffers and neither reads the
        // other. What does need ordering is the BLAS build below, which reads the buffers these copies
        // fill, and this flush is what provides it -- it blocks until the copies have completed, so the
        // build (recorded into its own submit inside createBLAS) cannot start before the data is there.
        // Submitting here rather than leaving it to the destructor at the end of the function is
        // therefore load bearing, not tidiness.
        //
        // Anything that later records the BLAS build into this batch must replace this flush with an
        // explicit vkCmdPipelineBarrier -- VK_PIPELINE_STAGE_TRANSFER_BIT / VK_ACCESS_TRANSFER_WRITE_BIT
        // to VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR /
        // VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR -- because within a single command buffer that
        // ordering is not implied, and it must keep both staging buffers alive past the later submit.
        batch.flush();

        geometry.vertexCount = vertexCount;
        geometry.indexCount = indexCount;

        if (device.rayTracingSupported())
        {
            // Alpha-tested geometry must stay non-opaque so the any-hit shader is invoked and can
            // discard transparent texels. Marking it opaque is what made every leaf billboard occlude
            // as a solid rectangle. Opaque is the faster path, so everything else keeps it.
            auto blas = AccelerationStructure::createBLAS(device, commandPool, *geometry.vertexBuffer,
                vertexCount, sVertexStride, VK_FORMAT_R32G32B32_SFLOAT, *geometry.indexBuffer, indexCount,
                VK_INDEX_TYPE_UINT32, !alphaTested);
            geometry.blas = std::make_unique<AccelerationStructure>(std::move(blas));
        }

        return geometry;
    }
}
