#include "vkcommands.hpp"

#include "vkcommon.hpp"
#include "vkdevice.hpp"

namespace Vk
{
    CommandPool::CommandPool(Device& device, uint32_t queueFamilyIndex)
        : mDevice(device)
    {
        VkCommandPoolCreateInfo poolInfo = {};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndex;

        VK_CHECK(vkCreateCommandPool(mDevice.handle(), &poolInfo, nullptr, &mPool));
    }

    CommandPool::~CommandPool()
    {
        if (mPool != VK_NULL_HANDLE)
            vkDestroyCommandPool(mDevice.handle(), mPool, nullptr);
    }

    std::vector<VkCommandBuffer> CommandPool::allocateMultiple(uint32_t count)
    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = mPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = count;

        std::vector<VkCommandBuffer> commandBuffers(count);
        VK_CHECK(vkAllocateCommandBuffers(mDevice.handle(), &allocInfo, commandBuffers.data()));
        return commandBuffers;
    }

}
