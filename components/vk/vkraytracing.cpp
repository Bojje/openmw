#include "vkraytracing.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

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

    static uint32_t findMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }
        throw std::runtime_error("Failed to find suitable memory type for acceleration structure");
    }

    static void createBuffer(VkDevice device, VkPhysicalDevice physicalDevice, VkDeviceSize size,
        VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
        VkBuffer& buffer, VkDeviceMemory& memory)
    {
        VkBufferCreateInfo bufferInfo = {};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VK_CHECK(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer));

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateFlagsInfo allocFlagsInfo = {};
        allocFlagsInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
        allocFlagsInfo.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;

        VkMemoryAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.pNext = &allocFlagsInfo;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(physicalDevice, memRequirements.memoryTypeBits, properties);

        VK_CHECK(vkAllocateMemory(device, &allocInfo, nullptr, &memory));
        VK_CHECK(vkBindBufferMemory(device, buffer, memory, 0));
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
        , mAccelerationStructure(other.mAccelerationStructure)
        , mBuffer(other.mBuffer)
        , mMemory(other.mMemory)
        , mDeviceAddress(other.mDeviceAddress)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mAccelerationStructure = VK_NULL_HANDLE;
        other.mBuffer = VK_NULL_HANDLE;
        other.mMemory = VK_NULL_HANDLE;
        other.mDeviceAddress = 0;
    }

    AccelerationStructure& AccelerationStructure::operator=(AccelerationStructure&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            mDevice = other.mDevice;
            mAccelerationStructure = other.mAccelerationStructure;
            mBuffer = other.mBuffer;
            mMemory = other.mMemory;
            mDeviceAddress = other.mDeviceAddress;
            other.mDevice = VK_NULL_HANDLE;
            other.mAccelerationStructure = VK_NULL_HANDLE;
            other.mBuffer = VK_NULL_HANDLE;
            other.mMemory = VK_NULL_HANDLE;
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
        if (mBuffer != VK_NULL_HANDLE && mDevice != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(mDevice, mBuffer, nullptr);
            mBuffer = VK_NULL_HANDLE;
        }
        if (mMemory != VK_NULL_HANDLE && mDevice != VK_NULL_HANDLE)
        {
            vkFreeMemory(mDevice, mMemory, nullptr);
            mMemory = VK_NULL_HANDLE;
        }
    }

    AccelerationStructure AccelerationStructure::createBLAS(Device& device, CommandPool& commandPool,
        Buffer& vertexBuffer, uint32_t vertexCount, VkDeviceSize vertexStride,
        VkFormat vertexFormat, Buffer& indexBuffer, uint32_t indexCount, VkIndexType indexType)
    {
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
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
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

        createBuffer(device.handle(), device.physical(), sizeInfo.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, as.mBuffer, as.mMemory);

        VkAccelerationStructureCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = as.mBuffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        VK_CHECK(rtFunctions().vkCreateAccelerationStructureKHR(device.handle(), &createInfo, nullptr, &as.mAccelerationStructure));

        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
        createBuffer(device.handle(), device.physical(), sizeInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory);

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
            vkDestroyBuffer(device.handle(), scratchBuffer, nullptr);
            vkFreeMemory(device.handle(), scratchMemory, nullptr);
            throw;
        }

        vkDestroyBuffer(device.handle(), scratchBuffer, nullptr);
        vkFreeMemory(device.handle(), scratchMemory, nullptr);

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
        VkDeviceMemory instanceStagingMemory = VK_NULL_HANDLE;
        createBuffer(device.handle(), device.physical(), instanceBufferSize,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            instanceStagingBuffer, instanceStagingMemory);

        void* data;
        VK_CHECK(vkMapMemory(device.handle(), instanceStagingMemory, 0, instanceBufferSize, 0, &data));
        std::memcpy(data, instances.data(), instanceBufferSize);
        vkUnmapMemory(device.handle(), instanceStagingMemory);

        VkBuffer instanceBuffer = VK_NULL_HANDLE;
        VkDeviceMemory instanceMemory = VK_NULL_HANDLE;
        try
        {
            createBuffer(device.handle(), device.physical(), instanceBufferSize,
                VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR
                    | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, instanceBuffer, instanceMemory);

            VkCommandBuffer copyCmd = commandPool.beginSingleTime();
            VkBufferCopy copyRegion = {};
            copyRegion.size = instanceBufferSize;
            vkCmdCopyBuffer(copyCmd, instanceStagingBuffer, instanceBuffer, 1, &copyRegion);
            commandPool.endSingleTime(copyCmd, device.graphicsQueue());
        }
        catch (...)
        {
            vkDestroyBuffer(device.handle(), instanceStagingBuffer, nullptr);
            vkFreeMemory(device.handle(), instanceStagingMemory, nullptr);
            if (instanceBuffer != VK_NULL_HANDLE)
                vkDestroyBuffer(device.handle(), instanceBuffer, nullptr);
            if (instanceMemory != VK_NULL_HANDLE)
                vkFreeMemory(device.handle(), instanceMemory, nullptr);
            throw;
        }

        vkDestroyBuffer(device.handle(), instanceStagingBuffer, nullptr);
        vkFreeMemory(device.handle(), instanceStagingMemory, nullptr);

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

        createBuffer(device.handle(), device.physical(), sizeInfo.accelerationStructureSize,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, as.mBuffer, as.mMemory);

        VkAccelerationStructureCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
        createInfo.buffer = as.mBuffer;
        createInfo.size = sizeInfo.accelerationStructureSize;
        createInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        VK_CHECK(rtFunctions().vkCreateAccelerationStructureKHR(device.handle(), &createInfo, nullptr, &as.mAccelerationStructure));

        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VkDeviceMemory scratchMemory = VK_NULL_HANDLE;
        createBuffer(device.handle(), device.physical(), sizeInfo.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, scratchBuffer, scratchMemory);

        buildInfo.dstAccelerationStructure = as.mAccelerationStructure;
        buildInfo.scratchData.deviceAddress = getBufferDeviceAddress(device.handle(), scratchBuffer);

        VkAccelerationStructureBuildRangeInfoKHR rangeInfo = {};
        rangeInfo.primitiveCount = instanceCount;
        const VkAccelerationStructureBuildRangeInfoKHR* pRangeInfo = &rangeInfo;

        try
        {
            VkCommandBuffer cmd = commandPool.beginSingleTime();
            rtFunctions().vkCmdBuildAccelerationStructuresKHR(cmd, 1, &buildInfo, &pRangeInfo);
            commandPool.endSingleTime(cmd, device.graphicsQueue());
        }
        catch (...)
        {
            vkDestroyBuffer(device.handle(), scratchBuffer, nullptr);
            vkFreeMemory(device.handle(), scratchMemory, nullptr);
            vkDestroyBuffer(device.handle(), instanceBuffer, nullptr);
            vkFreeMemory(device.handle(), instanceMemory, nullptr);
            throw;
        }

        vkDestroyBuffer(device.handle(), scratchBuffer, nullptr);
        vkFreeMemory(device.handle(), scratchMemory, nullptr);
        vkDestroyBuffer(device.handle(), instanceBuffer, nullptr);
        vkFreeMemory(device.handle(), instanceMemory, nullptr);

        VkAccelerationStructureDeviceAddressInfoKHR addressInfo = {};
        addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
        addressInfo.accelerationStructure = as.mAccelerationStructure;
        as.mDeviceAddress = rtFunctions().vkGetAccelerationStructureDeviceAddressKHR(device.handle(), &addressInfo);

        return as;
    }

    // RayTracingPipeline

    RayTracingPipeline::RayTracingPipeline(Device& device, VkDescriptorSetLayout descriptorLayout,
        ShaderModule& raygen, ShaderModule& miss, ShaderModule& shadowMiss,
        ShaderModule& closestHit)
        : mDevice(device)
    {
        VkPushConstantRange pushConstantRange = {};
        pushConstantRange.stageFlags = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        pushConstantRange.offset = 0;
        pushConstantRange.size = sizeof(CameraPushConstants);

        VkPipelineLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &descriptorLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushConstantRange;
        VK_CHECK(vkCreatePipelineLayout(device.handle(), &layoutInfo, nullptr, &mPipelineLayout));

        std::array<VkPipelineShaderStageCreateInfo, 4> stages = {};

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

        // Group 0: raygen
        // Group 1: miss (sky)
        // Group 2: miss (shadow)
        // Group 3: closest hit
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
        groups[3].anyHitShader = VK_SHADER_UNUSED_KHR;
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
            vkDestroyBuffer(dev, mSbtBuffer, nullptr);
        if (mSbtMemory != VK_NULL_HANDLE)
            vkFreeMemory(dev, mSbtMemory, nullptr);
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

        createBuffer(mDevice.handle(), mDevice.physical(), sbtSize,
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            mSbtBuffer, mSbtMemory);

        void* mapped;
        VK_CHECK(vkMapMemory(mDevice.handle(), mSbtMemory, 0, sbtSize, 0, &mapped));
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

        vkUnmapMemory(mDevice.handle(), mSbtMemory);

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
