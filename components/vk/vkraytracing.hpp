#ifndef OPENMW_COMPONENTS_VK_VKRAYTRACING_H
#define OPENMW_COMPONENTS_VK_VKRAYTRACING_H

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

// Forward declared for the same reason vkdevice.hpp does it: vk_mem_alloc.h is a 700 KB single-header
// library and nothing out here needs more than the handle types. Repeating the typedefs is harmless,
// the header itself declares them identically.
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vk
{
    class Device;
    class CommandPool;
    class Buffer;
    class ShaderModule;

    struct RayTracingFunctions
    {
        PFN_vkCreateAccelerationStructureKHR vkCreateAccelerationStructureKHR = nullptr;
        PFN_vkDestroyAccelerationStructureKHR vkDestroyAccelerationStructureKHR = nullptr;
        PFN_vkGetAccelerationStructureDeviceAddressKHR vkGetAccelerationStructureDeviceAddressKHR = nullptr;
        PFN_vkCmdBuildAccelerationStructuresKHR vkCmdBuildAccelerationStructuresKHR = nullptr;
        PFN_vkGetAccelerationStructureBuildSizesKHR vkGetAccelerationStructureBuildSizesKHR = nullptr;
        // The two halves of BLAS compaction: write the compacted size into a query pool once the build
        // is done, then copy the structure into a right-sized one.
        PFN_vkCmdWriteAccelerationStructuresPropertiesKHR vkCmdWriteAccelerationStructuresPropertiesKHR = nullptr;
        PFN_vkCmdCopyAccelerationStructureKHR vkCmdCopyAccelerationStructureKHR = nullptr;
        PFN_vkCreateRayTracingPipelinesKHR vkCreateRayTracingPipelinesKHR = nullptr;
        PFN_vkGetRayTracingShaderGroupHandlesKHR vkGetRayTracingShaderGroupHandlesKHR = nullptr;
        PFN_vkCmdTraceRaysKHR vkCmdTraceRaysKHR = nullptr;
    };

    void loadRayTracingFunctions(VkDevice device);
    const RayTracingFunctions& rtFunctions();

    class AccelerationStructure
    {
    public:
        ~AccelerationStructure();

        AccelerationStructure(const AccelerationStructure&) = delete;
        AccelerationStructure& operator=(const AccelerationStructure&) = delete;
        AccelerationStructure(AccelerationStructure&& other) noexcept;
        AccelerationStructure& operator=(AccelerationStructure&& other) noexcept;

        // opaque has no default on purpose: every call site must state whether its geometry is
        // alpha-tested, because getting it wrong is a silent visual bug rather than a compile error.
        static AccelerationStructure createBLAS(Device& device, CommandPool& commandPool,
            Buffer& vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
            VkFormat vertexFormat, Buffer& indexBuffer, uint32_t indexCount, VkIndexType indexType,
            bool opaque);

        static AccelerationStructure createTLAS(Device& device, CommandPool& commandPool,
            const std::vector<VkAccelerationStructureInstanceKHR>& instances);

        VkAccelerationStructureKHR handle() const { return mAccelerationStructure; }
        VkDeviceAddress deviceAddress() const { return mDeviceAddress; }

    private:
        AccelerationStructure() = default;

        void destroy();

        VkDevice mDevice = VK_NULL_HANDLE;
        // The allocator is stored per structure rather than looked up from a Device: destroy() runs
        // from the destructor, which has no Device reference to reach for.
        VmaAllocator mAllocator = VK_NULL_HANDLE;
        VkAccelerationStructureKHR mAccelerationStructure = VK_NULL_HANDLE;
        VkBuffer mBuffer = VK_NULL_HANDLE;
        VmaAllocation mAllocation = VK_NULL_HANDLE;
        VkDeviceAddress mDeviceAddress = 0;
    };

    // NOTE: the ray tracing pipeline deliberately uses no push constants. The raygen shader needs two
    // mat4 plus a vec4 (144 bytes), which exceeds the 128-byte maxPushConstantsSize that Vulkan
    // guarantees on every implementation, so that data is read from the scene UBO instead.

    class RayTracingPipeline
    {
    public:
        RayTracingPipeline(Device& device, VkDescriptorSetLayout descriptorLayout,
            ShaderModule& raygen, ShaderModule& miss, ShaderModule& shadowMiss,
            ShaderModule& closestHit, ShaderModule& anyHit);
        ~RayTracingPipeline();

        RayTracingPipeline(const RayTracingPipeline&) = delete;
        RayTracingPipeline& operator=(const RayTracingPipeline&) = delete;

        void bind(VkCommandBuffer cmd);
        void traceRays(VkCommandBuffer cmd, uint32_t width, uint32_t height);

        VkPipelineLayout layout() const { return mPipelineLayout; }
        VkPipeline handle() const { return mPipeline; }

    private:
        void createShaderBindingTable();

        Device& mDevice;
        VkPipeline mPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;

        VkBuffer mSbtBuffer = VK_NULL_HANDLE;
        VmaAllocation mSbtAllocation = VK_NULL_HANDLE;

        VkStridedDeviceAddressRegionKHR mRaygenRegion = {};
        VkStridedDeviceAddressRegionKHR mMissRegion = {};
        VkStridedDeviceAddressRegionKHR mHitRegion = {};
        VkStridedDeviceAddressRegionKHR mCallableRegion = {};

        uint32_t mHandleSize = 0;
        uint32_t mHandleAlignment = 0;
    };
}

#endif
