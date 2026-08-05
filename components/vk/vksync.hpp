#ifndef OPENMW_COMPONENTS_VK_VKSYNC_H
#define OPENMW_COMPONENTS_VK_VKSYNC_H

#include <array>
#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkcommon.hpp"

namespace Vk
{
    class Device;

    class FrameSync
    {
    public:
        FrameSync(Device& device, uint32_t swapchainImageCount);
        ~FrameSync();

        FrameSync(const FrameSync&) = delete;
        FrameSync& operator=(const FrameSync&) = delete;

        void waitForFrame(uint32_t frameIndex);
        void waitForImage(uint32_t imageIndex, uint32_t frameIndex);
        void resetFrame(uint32_t frameIndex);

        VkSemaphore imageAvailable(uint32_t frameIndex) const { return mImageAvailable[frameIndex]; }
        VkSemaphore renderFinished(uint32_t imageIndex) const { return mRenderFinished[imageIndex]; }
        VkFence inFlightFence(uint32_t frameIndex) const { return mInFlightFences[frameIndex]; }

        void resizeRenderFinished(uint32_t swapchainImageCount);

    private:
        void destroyRenderFinished();

        Device& mDevice;
        std::array<VkSemaphore, maxFramesInFlight> mImageAvailable = {};
        std::vector<VkSemaphore> mRenderFinished;
        std::vector<VkFence> mImagesInFlight;
        std::array<VkFence, maxFramesInFlight> mInFlightFences = {};
    };
}

#endif
