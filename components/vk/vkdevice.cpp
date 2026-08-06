#include "vkdevice.hpp"

#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "vkinstance.hpp"

namespace Vk
{
    const std::vector<const char*> Device::sRequiredExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    Device::Device(Instance& instance, VkSurfaceKHR surface)
    {
        selectPhysicalDevice(instance.handle(), surface);
        createLogicalDevice();
    }

    Device::~Device()
    {
        if (mDevice != VK_NULL_HANDLE)
            vkDestroyDevice(mDevice, nullptr);
    }

    Device::Device(Device&& other) noexcept
        : mDevice(other.mDevice)
        , mPhysicalDevice(other.mPhysicalDevice)
        , mQueueFamilyIndices(other.mQueueFamilyIndices)
        , mGraphicsQueue(other.mGraphicsQueue)
        , mPresentQueue(other.mPresentQueue)
    {
        other.mDevice = VK_NULL_HANDLE;
        other.mPhysicalDevice = VK_NULL_HANDLE;
        other.mGraphicsQueue = VK_NULL_HANDLE;
        other.mPresentQueue = VK_NULL_HANDLE;
    }

    Device& Device::operator=(Device&& other) noexcept
    {
        if (this != &other)
        {
            if (mDevice != VK_NULL_HANDLE)
                vkDestroyDevice(mDevice, nullptr);

            mDevice = other.mDevice;
            mPhysicalDevice = other.mPhysicalDevice;
            mQueueFamilyIndices = other.mQueueFamilyIndices;
            mGraphicsQueue = other.mGraphicsQueue;
            mPresentQueue = other.mPresentQueue;

            other.mDevice = VK_NULL_HANDLE;
            other.mPhysicalDevice = VK_NULL_HANDLE;
            other.mGraphicsQueue = VK_NULL_HANDLE;
            other.mPresentQueue = VK_NULL_HANDLE;
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
        for (auto device : devices)
        {
            auto queueIndices = findQueueFamilies(device, surface);
            if (!queueIndices.isComplete())
                continue;

            if (!checkDeviceExtensionSupport(device, sRequiredExtensions))
                continue;

            int score = rateDevice(device);

            if (score > bestScore)
            {
                bestDevice = device;
                bestScore = score;
            }
        }

        if (bestDevice == VK_NULL_HANDLE)
            throw std::runtime_error("No suitable Vulkan GPU found");

        mPhysicalDevice = bestDevice;
        mQueueFamilyIndices = findQueueFamilies(mPhysicalDevice, surface);

        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        std::clog << "Selected GPU: " << props.deviceName << '\n';
    }

    void Device::createLogicalDevice()
    {
        std::set<uint32_t> uniqueFamilies;
        uniqueFamilies.insert(mQueueFamilyIndices.graphics.value());
        uniqueFamilies.insert(mQueueFamilyIndices.present.value());

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

        std::vector<const char*> deviceExtensions(sRequiredExtensions);

        VkDeviceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        VK_CHECK(vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice));

        vkGetDeviceQueue(mDevice, mQueueFamilyIndices.graphics.value(), 0, &mGraphicsQueue);
        vkGetDeviceQueue(mDevice, mQueueFamilyIndices.present.value(), 0, &mPresentQueue);

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

            VkBool32 presentSupport = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
            if (presentSupport)
                indices.present = i;

            if (indices.isComplete())
                break;
        }

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

    int Device::rateDevice(VkPhysicalDevice device) const
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
