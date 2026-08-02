#include "vkdevice.hpp"

#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <components/debug/debuglog.hpp>

#include <vk_mem_alloc.h>

#include "vkinstance.hpp"

namespace Vk
{
    const std::vector<const char*> Device::sRequiredExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    const std::vector<const char*> Device::sRayTracingExtensions = {
        VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
        VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME,
        VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
    };

    Device::Device(Instance& instance, VkSurfaceKHR surface)
        : mSurface(surface)
    {
        selectPhysicalDevice(instance.handle(), surface);
        createLogicalDevice();
        createAllocator(instance);
    }

    void Device::createAllocator(Instance& instance)
    {
        VmaAllocatorCreateInfo info = {};
        info.physicalDevice = mPhysicalDevice;
        info.device = mDevice;
        info.instance = instance.handle();
        info.vulkanApiVersion = VK_API_VERSION_1_3;
        // The renderer enables VK_KHR_buffer_device_address whenever ray tracing is available, and VMA
        // has to be told so: without this flag it will not set VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
        // aside correctly and vkGetBufferDeviceAddress on a suballocated buffer misbehaves.
        if (mRayTracingSupported)
            info.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;

        VK_CHECK(vmaCreateAllocator(&info, &mAllocator));
    }

    Device::~Device()
    {
        // Before the device: every suballocation lives in memory this owns.
        if (mAllocator != VK_NULL_HANDLE)
            vmaDestroyAllocator(mAllocator);
        if (mDevice != VK_NULL_HANDLE)
            vkDestroyDevice(mDevice, nullptr);
    }

    Device::Device(Device&& other) noexcept
        : mDevice(other.mDevice)
        , mPhysicalDevice(other.mPhysicalDevice)
        , mSurface(other.mSurface)
        , mQueueFamilyIndices(other.mQueueFamilyIndices)
        , mGraphicsQueue(other.mGraphicsQueue)
        , mPresentQueue(other.mPresentQueue)
        , mComputeQueue(other.mComputeQueue)
        , mRayTracingSupported(other.mRayTracingSupported)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mPhysicalDevice = VK_NULL_HANDLE;
        other.mGraphicsQueue = VK_NULL_HANDLE;
        other.mPresentQueue = VK_NULL_HANDLE;
        other.mComputeQueue = VK_NULL_HANDLE;
    }

    Device& Device::operator=(Device&& other) noexcept
    {
        if (this != &other)
        {
            if (mDevice != VK_NULL_HANDLE)
                vkDestroyDevice(mDevice, nullptr);

            mDevice = other.mDevice;
            mPhysicalDevice = other.mPhysicalDevice;
            mSurface = other.mSurface;
            mQueueFamilyIndices = other.mQueueFamilyIndices;
            mGraphicsQueue = other.mGraphicsQueue;
            mPresentQueue = other.mPresentQueue;
            mComputeQueue = other.mComputeQueue;
            mRayTracingSupported = other.mRayTracingSupported;

            other.mDevice = VK_NULL_HANDLE;
            other.mPhysicalDevice = VK_NULL_HANDLE;
            other.mGraphicsQueue = VK_NULL_HANDLE;
            other.mPresentQueue = VK_NULL_HANDLE;
            other.mComputeQueue = VK_NULL_HANDLE;
        }
        return *this;
    }

    void Device::selectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface)
    {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0)
            throw std::runtime_error("No Vulkan-capable GPU found");

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        VkPhysicalDevice bestDevice = VK_NULL_HANDLE;
        int bestScore = -1;
        bool bestHasRT = false;

        for (auto device : devices)
        {
            auto queueIndices = findQueueFamilies(device, surface);
            if (!queueIndices.isComplete())
                continue;

            if (!checkDeviceExtensionSupport(device, sRequiredExtensions))
                continue;

            bool hasRT = checkDeviceExtensionSupport(device, sRayTracingExtensions);
            int score = rateDevice(device, surface);

            // Strongly prefer RT-capable devices
            if (hasRT && !bestHasRT)
            {
                bestDevice = device;
                bestScore = score;
                bestHasRT = true;
            }
            else if (hasRT == bestHasRT && score > bestScore)
            {
                bestDevice = device;
                bestScore = score;
                bestHasRT = hasRT;
            }
        }

        if (bestDevice == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable Vulkan GPU found");

        mPhysicalDevice = bestDevice;
        mRayTracingSupported = bestHasRT;
        mQueueFamilyIndices = findQueueFamilies(mPhysicalDevice, surface);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        Log(Debug::Info) << "Selected GPU: " << props.deviceName
                         << (mRayTracingSupported ? " (ray tracing supported)" : " (no ray tracing)");
    }

    void Device::createLogicalDevice()
    {
        std::set<uint32_t> uniqueFamilies;
        uniqueFamilies.insert(mQueueFamilyIndices.graphics.value());
        uniqueFamilies.insert(mQueueFamilyIndices.present.value());
        if (mQueueFamilyIndices.compute.has_value())
            uniqueFamilies.insert(mQueueFamilyIndices.compute.value());

        float queuePriority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        for (uint32_t family : uniqueFamilies)
        {
            VkDeviceQueueCreateInfo queueCreateInfo = {};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = family;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        // Collect all required extensions
        std::vector<const char*> deviceExtensions(sRequiredExtensions);
        if (mRayTracingSupported)
            deviceExtensions.insert(deviceExtensions.end(), sRayTracingExtensions.begin(), sRayTracingExtensions.end());

        // Feature chain for RT-capable devices
        VkPhysicalDeviceBufferDeviceAddressFeaturesKHR bufferDeviceAddressFeatures = {};
        bufferDeviceAddressFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR;
        bufferDeviceAddressFeatures.bufferDeviceAddress = VK_TRUE;

        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexingFeatures = {};
        descriptorIndexingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        descriptorIndexingFeatures.runtimeDescriptorArray = VK_TRUE;
        descriptorIndexingFeatures.descriptorBindingPartiallyBound = VK_TRUE;
        descriptorIndexingFeatures.descriptorBindingVariableDescriptorCount = VK_TRUE;
        descriptorIndexingFeatures.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelStructFeatures = {};
        accelStructFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;
        accelStructFeatures.accelerationStructure = VK_TRUE;

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures = {};
        rtPipelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
        rtPipelineFeatures.rayTracingPipeline = VK_TRUE;

        VkPhysicalDeviceFeatures supportedFeatures;
        vkGetPhysicalDeviceFeatures(mPhysicalDevice, &supportedFeatures);

        VkPhysicalDeviceFeatures2 deviceFeatures2 = {};
        deviceFeatures2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        deviceFeatures2.features.samplerAnisotropy = supportedFeatures.samplerAnisotropy;

        void** pNextChain = &deviceFeatures2.pNext;

        if (mRayTracingSupported)
        {
            *pNextChain = &bufferDeviceAddressFeatures;
            pNextChain = &bufferDeviceAddressFeatures.pNext;

            *pNextChain = &descriptorIndexingFeatures;
            pNextChain = &descriptorIndexingFeatures.pNext;

            *pNextChain = &accelStructFeatures;
            pNextChain = &accelStructFeatures.pNext;

            *pNextChain = &rtPipelineFeatures;
            pNextChain = &rtPipelineFeatures.pNext;
        }

        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.pNext = &deviceFeatures2;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        VK_CHECK(vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice));

        vkGetDeviceQueue(mDevice, mQueueFamilyIndices.graphics.value(), 0, &mGraphicsQueue);
        vkGetDeviceQueue(mDevice, mQueueFamilyIndices.present.value(), 0, &mPresentQueue);

        if (mQueueFamilyIndices.compute.has_value())
            vkGetDeviceQueue(mDevice, mQueueFamilyIndices.compute.value(), 0, &mComputeQueue);
        else
            vkGetDeviceQueue(mDevice, mQueueFamilyIndices.graphics.value(), 0, &mComputeQueue);
    }

    QueueFamilyIndices Device::findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) const
    {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        for (uint32_t i = 0; i < queueFamilyCount; ++i)
        {
            if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
                indices.graphics = i;

            if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
                indices.compute = i;

            if ((queueFamilies[i].queueFlags & VK_QUEUE_TRANSFER_BIT)
                && !(queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                indices.transfer = i;

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport)
                indices.present = i;

            if (indices.isComplete() && indices.compute.has_value())
                break;
        }

        // Use graphics queue for transfer if no dedicated transfer queue exists
        if (!indices.transfer.has_value() && indices.graphics.has_value())
            indices.transfer = indices.graphics;

        return indices;
    }

    bool Device::checkDeviceExtensionSupport(
        VkPhysicalDevice device, const std::vector<const char*>& extensions) const
    {
        uint32_t extensionCount = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> available(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, available.data());

        for (const char* required : extensions)
        {
            bool found = false;
            for (const auto& ext : available)
            {
                if (std::strcmp(ext.extensionName, required) == 0)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
        return true;
    }

    int Device::rateDevice(VkPhysicalDevice device, VkSurfaceKHR surface) const
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);

        int score = 0;

        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            score += 10000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
            score += 1000;

        score += static_cast<int>(props.limits.maxImageDimension2D);

        return score;
    }

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR Device::rayTracingProperties() const
    {
        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProperties = {};
        rtProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;

        VkPhysicalDeviceProperties2 props2 = {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &rtProperties;

        vkGetPhysicalDeviceProperties2(mPhysicalDevice, &props2);
        return rtProperties;
    }

    uint32_t Device::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(mPhysicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
                return i;
        }

        throw std::runtime_error("Failed to find suitable memory type");
    }
}
