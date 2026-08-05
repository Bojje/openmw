#ifndef OPENMW_COMPONENTS_VK_VKCOMMANDS_H
#define OPENMW_COMPONENTS_VK_VKCOMMANDS_H

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;

    class CommandPool
    {
    public:
        CommandPool(Device& device, uint32_t queueFamilyIndex);
        ~CommandPool();

        CommandPool(const CommandPool&) = delete;
        CommandPool& operator=(const CommandPool&) = delete;

        std::vector<VkCommandBuffer> allocateMultiple(uint32_t count);

    private:
        Device& mDevice;
        VkCommandPool mPool = VK_NULL_HANDLE;
    };
}

#endif
