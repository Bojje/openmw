#include "vkcommands.hpp"

#include <stdexcept>

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

    VkCommandBuffer CommandPool::allocate()
    {
        VkCommandBufferAllocateInfo allocInfo = {};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = mPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        VK_CHECK(vkAllocateCommandBuffers(mDevice.handle(), &allocInfo, &commandBuffer));
        return commandBuffer;
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

    VkCommandBuffer CommandPool::beginSingleTime()
    {
        VkCommandBuffer commandBuffer = allocate();

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
        return commandBuffer;
    }

    void CommandPool::endSingleTime(VkCommandBuffer commandBuffer, VkQueue queue)
    {
        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        VK_CHECK(vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE));
        vkQueueWaitIdle(queue);

        vkFreeCommandBuffers(mDevice.handle(), mPool, 1, &commandBuffer);
    }
}
