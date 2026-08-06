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

        VkDevice handle() const { return mDevice; }
        VkPhysicalDevice physical() const { return mPhysicalDevice; }
        QueueFamilyIndices indices() const { return mQueueFamilyIndices; }
        VkQueue graphicsQueue() const { return mGraphicsQueue; }
        VkQueue presentQueue() const { return mPresentQueue; }

        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;

    private:
        void selectPhysicalDevice(VkInstance instance, VkSurfaceKHR surface);
        void createLogicalDevice();
        QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) const;
        bool checkDeviceExtensionSupport(VkPhysicalDevice device, const std::vector<const char*>& extensions) const;
        bool supportsDescriptorBudget(VkPhysicalDevice device) const;
        int rateDevice(VkPhysicalDevice device) const;

        VkDevice mDevice = VK_NULL_HANDLE;
        VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
        QueueFamilyIndices mQueueFamilyIndices;
        VkQueue mGraphicsQueue = VK_NULL_HANDLE;
        VkQueue mPresentQueue = VK_NULL_HANDLE;

        static const std::vector<const char*> sRequiredExtensions;
    };
}

#endif
