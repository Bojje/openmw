#include "vkraytracing.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

#include <vk_mem_alloc.h>

#include "vkbuffer.hpp"
#include "vkcommands.hpp"
#include "vkcommon.hpp"
#include "vkdevice.hpp"
#include "vkshader.hpp"

namespace Vk
{
    static RayTracingFunctions sRtFunctions;

    void loadRayTracingFunctions(VkDevice device)
    {
        auto load = [device](const char* name) {
            return vkGetDeviceProcAddr(device, name);
        };

        sRtFunctions.vkCreateAccelerationStructureKHR =
            reinterpret_cast<PFN_vkCreateAccelerationStructureKHR>(load("vkCreateAccelerationStructureKHR"));
        sRtFunctions.vkDestroyAccelerationStructureKHR =
            reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(load("vkDestroyAccelerationStructureKHR"));
        sRtFunctions.vkGetAccelerationStructureDeviceAddressKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(load("vkGetAccelerationStructureDeviceAddressKHR"));
        sRtFunctions.vkCmdBuildAccelerationStructuresKHR =
            reinterpret_cast<PFN_vkCmdBuildAccelerationStructuresKHR>(load("vkCmdBuildAccelerationStructuresKHR"));
        sRtFunctions.vkGetAccelerationStructureBuildSizesKHR =
            reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(load("vkGetAccelerationStructureBuildSizesKHR"));
        sRtFunctions.vkCreateRayTracingPipelinesKHR =
            reinterpret_cast<PFN_vkCreateRayTracingPipelinesKHR>(load("vkCreateRayTracingPipelinesKHR"));
        sRtFunctions.vkGetRayTracingShaderGroupHandlesKHR =
            reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(load("vkGetRayTracingShaderGroupHandlesKHR"));
        sRtFunctions.vkCmdTraceRaysKHR =
            reinterpret_cast<PFN_vkCmdTraceRaysKHR>(load("vkCmdTraceRaysKHR"));
    }

    const RayTracingFunctions& rtFunctions()
    {
        return sRtFunctions;
    }

    // Every buffer here is a VMA suballocation out of a shared block. The old path was one
    // vkAllocateMemory per buffer, and a cell load builds ~1450 BLASes that each need an AS buffer plus
    // a scratch buffer thrown away immediately after the build, so it burned ~2900 allocation calls
    // against a maxMemoryAllocationCount that is commonly 4096.
    //
    // properties still names the memory properties the buffer must have; VMA ORs them into its own
    // requirements. minAlignment is for the buffers whose device address has an alignment rule that
    // VkMemoryRequirements does not express - build scratch and the shader binding table. A dedicated
    // vkAllocateMemory satisfied those by accident because the buffer always sat at offset 0 of a
    // fresh allocation; a suballocation lands wherever the block has room.
    static VmaAllocation createBuffer(const Device& device, VkDeviceSize size,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        VmaAllocationCreateFlags allocFlags, VkBuffer& buffer, VkDeviceSize minAlignment = 0)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        // No VkMemoryAllocateFlagsInfo chaining for VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT: the
        // allocator was created with VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT, so it puts
        // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT on every block it allocates.
        VmaAllocationCreateInfo allocInfo = {};
        allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocInfo.requiredFlags = properties;
        allocInfo.flags = allocFlags;

        VmaAllocation allocation = VK_NULL_HANDLE;
        if (minAlignment > 0)
            VK_CHECK(vmaCreateBufferWithAlignment(device.allocator(), &bufferInfo, &allocInfo,
                minAlignment, &buffer, &allocation, nullptr));
        else
            VK_CHECK(vmaCreateBuffer(device.allocator(), &bufferInfo, &allocInfo, &buffer, &allocation, nullptr));

        return allocation;
    }

    // A build scratch buffer's device address must be a multiple of
    // minAccelerationStructureScratchOffsetAlignment, which lives in the acceleration structure
    // properties rather than in the buffer's memory requirements.
    static VkDeviceSize scratchAlignment(const Device& device)
    {
        VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties = {};
        asProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;

        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &asProperties;

        vkGetPhysicalDeviceProperties2(device.physical(), &props2);
        return asProperties.minAccelerationStructureScratchOffsetAlignment;
    }

    static VkDeviceAddress getBufferDeviceAddress(VkDevice device, VkBuffer buffer)
    {
        VkBufferDeviceAddressInfo addressInfo = {};
        addressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
        addressInfo.buffer = buffer;
        return vkGetBufferDeviceAddress(device, &addressInfo);
    }

    // AccelerationStructure

    AccelerationStructure::~AccelerationStructure()
    {
        destroy();
    }

    AccelerationStructure::AccelerationStructure(AccelerationStructure&& other) noexcept
        : mDevice(other.mDevice)
        , mAllocator(other.mAllocator)
        , mAccelerationStructure(other.mAccelerationStructure)
        , mBuffer(other.mBuffer)
        , mAllocation(other.mAllocation)
        , mDeviceAddress(other.mDeviceAddress)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mAllocator = VK_NULL_HANDLE;
        other.mAccelerationStructure = VK_NULL_HANDLE;
        other.mBuffer = VK_NULL_HANDLE;
        other.mAllocation = VK_NULL_HANDLE;
        other.mDeviceAddress = 0;
    }

    AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mAllocator = other.mAllocator;
            mAccelerationStructure = other.mAccelerationStructure;
            mBuffer = other.mBuffer;
            mAllocation = other.mAllocation;
            mDeviceAddress = other.mDeviceAddress;
            other.mDevice = VK_NULL_HANDLE;
            other.mAllocator = VK_NULL_HANDLE;
            other.mAccelerationStructure = VK_NULL_HANDLE;
            other.mBuffer = VK_NULL_HANDLE;
            other.mAllocation = VK_NULL_HANDLE;
            other.mDeviceAddress = 0;
        }
        return *this;
    }

    void AccelerationStructure::destroy()
    {
        if (mAccelerationStructure != VK_NULL_HANDLE && mDevice != VK_NULL_HANDLE)
        {
            rtFunctions().vkDestroyAccelerationStructureKHR(mDevice, mAccelerationStructure, nullptr);
            mAccelerationStructure = VK_NULL_HANDLE;
        }
        // vmaDestroyBuffer does the vkDestroyBuffer and returns the suballocation in one call, so the
        // buffer and its allocation always go together.
        if (mBuffer != VK_NULL_HANDLE && mAllocator != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(mAllocator, mBuffer, mAllocation);
            mBuffer = VK_NULL_HANDLE;
            mAllocation = VK_NULL_HANDLE;
        }
    }

    AccelerationStructure AccelerationStructure::createBLAS(Device& device, CommandPool& commandPool,
        Buffer& vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
        VkFormat vertexFormat, Buffer& indexBuffer, uint32_t indexCount, VkIndexType indexType,
        bool opaque)
    {
        // maxVertex below is vertexCount - 1, which wraps to 0xFFFFFFFF on an empty mesh and tells the
        // driver it may read four billion vertices out of the buffer. Callers guard against this today,
        // but the failure mode is silent memory corruption during the build, so refuse it here too.
        if (vertexCount == 0)
            throw std::runtime_error("createBLAS called with vertexCount == 0");

        VkAccelerationStructureGeometryTrianglesDataKHR triangles = {};
        triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
        triangles.vertexFormat = vertexFormat;
        triangles.vertexData.deviceAddress = getBufferDeviceAddress(device.handle(), vertexBuffer.handle());
        triangles.vertexStride = vertexStride;
        triangles.maxVertex = vertexCount - 1;
        triangles.indexType = indexType;
        triangles.indexData.deviceAddress = getBufferDeviceAddress(device.handle(), indexBuffer.handle());

        VkAccelerationStructureGeometryKHR geometry = {};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        // VK_GEOMETRY_OPAQUE_BIT_KHR lets the traversal accept a hit without ever invoking the any-hit
        // shader, so flagging everything opaque is what made alpha-tested leaf billboards occlude as
        // solid rectangles: their cut-out texels were never sampled. Clearing the bit is the only way
        // the any-hit shader gets a chance to discard those texels. Opaque is still the fast path and
        // remains the right choice for ordinary geometry - only alpha-tested meshes should pass false.
        geometry.flags = opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        geometry.geometry.triangles = triangles;

        uint32_t primitiveCount = indexCount / 3;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
        sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        rtFunctions().vkGetAccelerationStructureBuildSizesKHR(device.handle(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &primitiveCount, &sizeInfo);

        AccelerationStructure as;
        as.mDevice = device.handle();
        as.mAllocator = device.allocator();

        as.mAllocation = createBuffer(device, sizeInfo.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, as.mBuffer);

        VkAccelerationStructureCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = as.mBuffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(rtFunctions().vkCreateAccelerationStructureKHR(device.handle(), &createInfo, nullptr, &as.mAccelerationStructure));

        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = createBuffer(device, sizeInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, scratchBuffer, scratchAlignment(device));

        buildInfo.dstAccelerationStructure = as.mAccelerationStructure;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device.handle(), scratchBuffer);

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
        rangeInfo.primitiveCount = primitiveCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

        try
        {
            VkCommandBuffer cmd = commandPool.beginSingleTime();
            rtFunctions().vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
            commandPool.endSingleTime(cmd, device.graphicsQueue());
        }
        catch (...)
        {
            // as owns its buffer and handle and unwinding runs its destructor, so only the scratch
            // buffer needs cleaning up here.
            vmaDestroyBuffer(device.allocator(), scratchBuffer, scratchAllocation);
            throw;
        }

        vmaDestroyBuffer(device.allocator(), scratchBuffer, scratchAllocation);

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
        addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addressInfo.accelerationStructure = as.mAccelerationStructure;
        as.mDeviceAddress = rtFunctions().vkGetAccelerationStructureDeviceAddressKHR(device.handle(), &addressInfo);

        return as;
    }

    AccelerationStructure AccelerationStructure::createTLAS(Device& device, CommandPool& commandPool,
        const std::vector<VkAccelerationStructureInstanceKHR>& instances)
    {
        VkDeviceSize instanceBufferSize = sizeof(VkAccelerationStructureInstanceKHR) * instances.size();

        VkBuffer instanceStagingBuffer = VK_NULL_HANDLE;
        VmaAllocation instanceStagingAllocation = createBuffer(device, instanceBufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, instanceStagingBuffer);

        void* data;
        VK_CHECK(vmaMapMemory(device.allocator(), instanceStagingAllocation, &data));
        std::memcpy(data, instances.data(), instanceBufferSize);
        vmaUnmapMemory(device.allocator(), instanceStagingAllocation);

        VkBuffer instanceBuffer = VK_NULL_HANDLE;
        VmaAllocation instanceAllocation = VK_NULL_HANDLE;
        try
        {
            instanceAllocation = createBuffer(device, instanceBufferSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, instanceBuffer);

            VkCommandBuffer copyCmd = commandPool.beginSingleTime();
            VkBufferCopy copyRegion = {};
            copyRegion.size = instanceBufferSize;
            vkCmdCopyBuffer(copyCmd, instanceStagingBuffer, instanceBuffer, 1, &copyRegion);
            commandPool.endSingleTime(copyCmd, device.graphicsQueue());
        }
        catch (...)
        {
            vmaDestroyBuffer(device.allocator(), instanceStagingBuffer, instanceStagingAllocation);
            if (instanceBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(device.allocator(), instanceBuffer, instanceAllocation);
            throw;
        }

        vmaDestroyBuffer(device.allocator(), instanceStagingBuffer, instanceStagingAllocation);

        VkAccelerationStructureGeometryInstancesDataKHR instancesData = {};
        instancesData.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
        instancesData.data.deviceAddress = getBufferDeviceAddress(device.handle(), instanceBuffer);

        VkAccelerationStructureGeometryKHR geometry = {};
        geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
        geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        geometry.geometry.instances = instancesData;

        uint32_t instanceCount = static_cast<uint32_t>(instances.size());

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo = {};
        buildInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR
            | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;

        VkAccelerationStructureBuildSizesInfoKHR sizeInfo = {};
        sizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        rtFunctions().vkGetAccelerationStructureBuildSizesKHR(device.handle(),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo, &instanceCount, &sizeInfo);

        AccelerationStructure as;
        as.mDevice = device.handle();
        as.mAllocator = device.allocator();

        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = VK_NULL_HANDLE;

        // The instance buffer must live until the build has been submitted and waited on, and every
        // step from here to that point can throw: the AS buffer allocation, the AS creation, the
        // scratch allocation and the build itself. One catch covers them all - previously only the
        // build was guarded, so a failure in the two steps before it leaked the instance buffer. as
        // owns its own buffer and handle and unwinding destroys it.
        try
        {
            as.mAllocation = createBuffer(device, sizeInfo.accelerationStructureSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, as.mBuffer);

            VkAccelerationStructureCreateInfoKHR createInfo = {};
            createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
            createInfo.buffer = as.mBuffer;
            createInfo.size = sizeInfo.accelerationStructureSize;
            createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
            VK_CHECK(rtFunctions().vkCreateAccelerationStructureKHR(device.handle(), &createInfo, nullptr, &as.mAccelerationStructure));

            scratchAllocation = createBuffer(device, sizeInfo.buildScratchSize,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, scratchBuffer, scratchAlignment(device));

            buildInfo.dstAccelerationStructure = as.mAccelerationStructure;
            buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device.handle(), scratchBuffer);

            VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
            rangeInfo.primitiveCount = instanceCount;
            const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

            VkCommandBuffer cmd = commandPool.beginSingleTime();
            rtFunctions().vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
            commandPool.endSingleTime(cmd, device.graphicsQueue());
        }
        catch (...)
        {
            if (scratchBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(device.allocator(), scratchBuffer, scratchAllocation);
            vmaDestroyBuffer(device.allocator(), instanceBuffer, instanceAllocation);
            throw;
        }

        vmaDestroyBuffer(device.allocator(), scratchBuffer, scratchAllocation);
        vmaDestroyBuffer(device.allocator(), instanceBuffer, instanceAllocation);

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
        addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addressInfo.accelerationStructure = as.mAccelerationStructure;
        as.mDeviceAddress = rtFunctions().vkGetAccelerationStructureDeviceAddressKHR(device.handle(), &addressInfo);

        return as;
    }

    // RayTracingPipeline

    RayTracingPipeline::RayTracingPipeline(Device& device, VkDescriptorSetLayout descriptorLayout,
        ShaderModule& raygen, ShaderModule& miss, ShaderModule& shadowMiss,
        ShaderModule& closestHit, ShaderModule& anyHit)
        : mDevice(device)
    {
        // No push constants: raygen reads its camera matrices and sun direction from the scene UBO
        // (binding 5) because 144 bytes would exceed the guaranteed 128-byte push constant limit.
        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout;
        layoutInfo.pushConstantRangeCount = 0;
        VK_CHECK(vkCreatePipelineLayout(device.handle(), &layoutInfo, nullptr, &mPipelineLayout));

        std::array<VkPipelineShaderStageCreateInfo, 5> stages = {};

        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        stages[0].module = raygen.handle();
        stages[0].pName = "main";

        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        stages[1].module = miss.handle();
        stages[1].pName = "main";

        stages[2].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[2].stage = VK_SHADER_STAGE_MISS_BIT_KHR;
        stages[2].module = shadowMiss.handle();
        stages[2].pName = "main";

        stages[3].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[3].stage = VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        stages[3].module = closestHit.handle();
        stages[3].pName = "main";

        stages[4].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[4].stage = VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        stages[4].module = anyHit.handle();
        stages[4].pName = "main";

        // Group 0: raygen
        // Group 1: miss (sky)
        // Group 2: miss (shadow)
        // Group 3: hit group (closest hit + any hit)
        //
        // The any-hit shader joins the existing triangles hit group rather than forming a new one: a
        // hit group is one SBT record that names up to a closest-hit, an any-hit and an intersection
        // shader. So there are 5 stages but still only 4 groups, and hitCount in
        // createShaderBindingTable stays 1. Bumping it because "we added a shader" would misalign
        // every region offset in the SBT.
        std::array<VkRayTracingShaderGroupCreateInfoKHR, 4> groups = {};

        groups[0].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[0].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[0].generalShader = 0;
        groups[0].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[0].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[1].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[1].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[1].generalShader = 1;
        groups[1].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[1].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[2].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[2].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR;
        groups[2].generalShader = 2;
        groups[2].closestHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].anyHitShader = VK_SHADER_UNUSED_KHR;
        groups[2].intersectionShader = VK_SHADER_UNUSED_KHR;

        groups[3].sType = VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR;
        groups[3].type = VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
        groups[3].generalShader = VK_SHADER_UNUSED_KHR;
        groups[3].closestHitShader = 3;
        groups[3].anyHitShader = 4;
        groups[3].intersectionShader = VK_SHADER_UNUSED_KHR;

        VkRayTracingPipelineCreateInfoKHR pipelineInfo = {};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.groupCount = static_cast<uint32_t>(groups.size());
        pipelineInfo.pGroups = groups.data();
        auto rtProps = device.rayTracingProperties();

        pipelineInfo.maxPipelineRayRecursionDepth = 1;
        if (pipelineInfo.maxPipelineRayRecursionDepth > rtProps.maxRayRecursionDepth)
            throw std::runtime_error("Device maxRayRecursionDepth (" + std::to_string(rtProps.maxRayRecursionDepth)
                + ") is less than required (" + std::to_string(pipelineInfo.maxPipelineRayRecursionDepth) + ")");
        pipelineInfo.layout = mPipelineLayout;

        VK_CHECK(rtFunctions().vkCreateRayTracingPipelinesKHR(
            device.handle(), VK_NULL_HANDLE, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mPipeline));

        mHandleSize = rtProps.shaderGroupHandleSize;
        mHandleAlignment = rtProps.shaderGroupHandleAlignment;

        createShaderBindingTable();
    }

    RayTracingPipeline::~RayTracingPipeline()
    {
        VkDevice dev = mDevice.handle();
        if (mSbtBuffer != VK_NULL_HANDLE)
            vmaDestroyBuffer(mDevice.allocator(), mSbtBuffer, mSbtAllocation);
        if (mPipeline != VK_NULL_HANDLE)
            vkDestroyPipeline(dev, mPipeline, nullptr);
        if (mPipelineLayout != VK_NULL_HANDLE)
            vkDestroyPipelineLayout(dev, mPipelineLayout, nullptr);
    }

    void RayTracingPipeline::createShaderBindingTable()
    {
        auto rtProps = mDevice.rayTracingProperties();
        uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;

        auto align = [](uint32_t size, uint32_t alignment) -> uint32_t {
            return (size + alignment - 1) & ~(alignment - 1);
        };

        uint32_t handleSizeAligned = align(mHandleSize, mHandleAlignment);

        constexpr uint32_t raygenCount = 1;
        constexpr uint32_t missCount = 2;
        constexpr uint32_t hitCount = 1;
        constexpr uint32_t groupCount = raygenCount + missCount + hitCount;

        uint32_t raygenRegionSize = align(handleSizeAligned * raygenCount, baseAlignment);
        uint32_t missRegionSize = align(handleSizeAligned * missCount, baseAlignment);
        uint32_t hitRegionSize = align(handleSizeAligned * hitCount, baseAlignment);
        uint32_t sbtSize = raygenRegionSize + missRegionSize + hitRegionSize;

        std::vector<uint8_t> handles(mHandleSize * groupCount);
        VK_CHECK(rtFunctions().vkGetRayTracingShaderGroupHandlesKHR(
            mDevice.handle(), mPipeline, 0, groupCount, handles.size(), handles.data()));

        // Written once, front to back, and never read back on the host - hence sequential write.
        // baseAlignment is passed as the minimum: vkCmdTraceRaysKHR requires each region's device
        // address to be a multiple of shaderGroupBaseAlignment, and the region addresses are this
        // buffer's address plus offsets that are already multiples of it. A suballocation only
        // guarantees the buffer's own VkMemoryRequirements alignment, which can be smaller.
        mSbtAllocation = createBuffer(mDevice, sbtSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, mSbtBuffer, baseAlignment);

        void* mapped;
        VK_CHECK(vmaMapMemory(mDevice.allocator(), mSbtAllocation, &mapped));
        auto* sbtData = static_cast<uint8_t*>(mapped);

        // Raygen at offset 0
        std::memcpy(sbtData, handles.data(), mHandleSize);

        // Miss shaders at offset raygenRegionSize
        uint8_t* missData = sbtData + raygenRegionSize;
        for (uint32_t i = 0; i < missCount; i++)
            std::memcpy(missData + i * handleSizeAligned, handles.data() + (raygenCount + i) * mHandleSize, mHandleSize);

        // Hit shaders at offset raygenRegionSize + missRegionSize
        uint8_t* hitData = sbtData + raygenRegionSize + missRegionSize;
        for (uint32_t i = 0; i < hitCount; i++)
            std::memcpy(hitData + i * handleSizeAligned, handles.data() + (raygenCount + missCount + i) * mHandleSize, mHandleSize);

        vmaUnmapMemory(mDevice.allocator(), mSbtAllocation);

        VkDeviceAddress sbtAddress = getBufferDeviceAddress(mDevice.handle(), mSbtBuffer);

        mRaygenRegion.deviceAddress = sbtAddress;
        mRaygenRegion.stride = handleSizeAligned;
        mRaygenRegion.size = handleSizeAligned;

        mMissRegion.deviceAddress = sbtAddress + raygenRegionSize;
        mMissRegion.stride = handleSizeAligned;
        mMissRegion.size = missRegionSize;

        mHitRegion.deviceAddress = sbtAddress + raygenRegionSize + missRegionSize;
        mHitRegion.stride = handleSizeAligned;
        mHitRegion.size = hitRegionSize;

        mCallableRegion = {};
    }

    void RayTracingPipeline::bind(VkCommandBuffer cmd)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, mPipeline);
    }

    void RayTracingPipeline::traceRays(VkCommandBuffer cmd, uint32_t width, uint32_t height)
    {
        rtFunctions().vkCmdTraceRaysKHR(cmd, &mRaygenRegion, &mMissRegion, &mHitRegion, &mCallableRegion,
            width, height, 1);
    }
}
