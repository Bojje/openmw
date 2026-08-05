#ifndef OPENMW_COMPONENTS_VK_VKDEVICE_H
#define OPENMW_COMPONENTS_VK_VKDEVICE_H

#include <vector>

#include <vulkan/vulkan.h>

#include "vkcommon.hpp"

namespace Vk
{
    class Instance;

    class Device
    {
    public:
        Device(Instance& instance, VkSurfaceKHR surface);
        ~Device();

        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&& other) noexcept;
        Device& operator=(Device&& other) noexcept;

        VkDevice handle() const { return mDevice; }
        VkPhysicalDevice physical() const { return mPhysicalDevice; }
        QueueFamilyIndices indices() const { return mQueueFamilyIndices; }
        VkQueue graphicsQueue() const { return mGraphicsQueue; }
        VkQueue presentQueue() const { return mPresentQueue; }
        VkQueue computeQueue() const { return mComputeQueue; }
        bool rayTracingSupported() const { return mRayTracingSupported; }

        VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingProperties() const;
        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    private:
        void selectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);
        void createLogicalDevice();
        QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) const;
        bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*>& extensions) const;
        int rateDevice(VkPhysicalDevice device, VkSurfaceKHR surface) const;

        VkDevice mDevice = VK_NULL_HANDLE;
        VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;
        QueueFamilyIndices mQueueFamilyIndices;
        VkQueue mGraphicsQueue = VK_NULL_HANDLE;
        VkQueue mPresentQueue = VK_NULL_HANDLE;
        VkQueue mComputeQueue = VK_NULL_HANDLE;
        bool mRayTracingSupported = false;

        static const std::vector<const char*> sRequiredExtensions;
        static const std::vector<const char*> sRayTracingExtensions;
    };
}

#endif
