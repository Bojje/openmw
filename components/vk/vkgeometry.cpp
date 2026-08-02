#include "vkgeometry.hpp"

#include "vkbuffer.hpp"
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

    Geometry uploadGeometry(Device& device, CommandPool& commandPool, const float* vertexData,
        uint32_t vertexCount, const uint32_t* indices, uint32_t indexCount)
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

        auto vb = Buffer::createWithStaging(device, commandPool, vertexFlags, vertexData, vertexSize);
        geometry.vertexBuffer = std::make_unique<Buffer>(std::move(vb));

        auto ib = Buffer::createWithStaging(device, commandPool, indexFlags, indices, indexSize);
        geometry.indexBuffer = std::make_unique<Buffer>(std::move(ib));

        geometry.vertexCount = vertexCount;
        geometry.indexCount = indexCount;

        if (device.rayTracingSupported())
        {
            auto blas = AccelerationStructure::createBLAS(device, commandPool, *geometry.vertexBuffer,
                vertexCount, sVertexStride, VK_FORMAT_R32G32B32_SFLOAT, *geometry.indexBuffer, indexCount,
                VK_INDEX_TYPE_UINT32);
            geometry.blas = std::make_unique<AccelerationStructure>(std::move(blas));
        }

        return geometry;
    }
}
