#include "vksync.hpp"

#include <limits>
#include <stdexcept>

#include "vkdevice.hpp"

namespace Vk
{
    FrameSync::FrameSync(Device& device, uint32_t swapchainImageCount)
        : mDevice(device)
    {
        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo = {};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (uint32_t i = 0; i < maxFramesInFlight; ++i)
        {
            VK_CHECK(vkCreateSemaphore(mDevice.handle(), &semaphoreInfo, nullptr, &mImageAvailable[i]));
            VK_CHECK(vkCreateFence(mDevice.handle(), &fenceInfo, nullptr, &mInFlightFences[i]));
        }

        resizeImageSemaphores(swapchainImageCount);
    }

    FrameSync::~FrameSync()
    {
        destroyImageSemaphores();

        for (uint32_t i = 0; i < maxFramesInFlight; ++i)
        {
            if (mImageAvailable[i] != VK_NULL_HANDLE)
                vkDestroySemaphore(mDevice.handle(), mImageAvailable[i], nullptr);
            if (mInFlightFences[i] != VK_NULL_HANDLE)
                vkDestroyFence(mDevice.handle(), mInFlightFences[i], nullptr);
        }
    }

    void FrameSync::destroyImageSemaphores()
    {
        for (VkSemaphore semaphore : mRenderFinished)
        {
            if (semaphore != VK_NULL_HANDLE)
                vkDestroySemaphore(mDevice.handle(), semaphore, nullptr);
        }
        mRenderFinished.clear();
    }

    void FrameSync::resizeImageSemaphores(uint32_t swapchainImageCount)
    {
        destroyImageSemaphores();

        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        mRenderFinished.resize(swapchainImageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < swapchainImageCount; ++i)
            VK_CHECK(vkCreateSemaphore(mDevice.handle(), &semaphoreInfo, nullptr, &mRenderFinished[i]));
    }

    void FrameSync::waitForFrame(uint32_t frameIndex)
    {
        VK_CHECK(vkWaitForFences(
            mDevice.handle(), 1, &mInFlightFences[frameIndex], VK_TRUE, std::numeric_limits<uint64_t>::max()));
    }

    void FrameSync::resetFrame(uint32_t frameIndex)
    {
        VK_CHECK(vkResetFences(mDevice.handle(), 1, &mInFlightFences[frameIndex]));
    }
}
