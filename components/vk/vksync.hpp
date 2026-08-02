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
        // swapchainImageCount is required because the render-finished semaphores must be owned per
        // swapchain image, not per frame in flight: a presentation engine may still be waiting on the
        // semaphore of a previously presented image when a later frame reuses that frame-in-flight
        // slot. Reusing them per frame violates VUID-vkQueueSubmit-pSignalSemaphores-00067.
        FrameSync(Device& device, uint32_t swapchainImageCount);
        ~FrameSync();

        FrameSync(const FrameSync&) = delete;
        FrameSync& operator=(const FrameSync&) = delete;

        void waitForFrame(uint32_t frameIndex);
        void resetFrame(uint32_t frameIndex);

        // Recreates the per-image semaphores when the swapchain image count changes.
        // The caller must have idled the device first.
        void resizeImageSemaphores(uint32_t swapchainImageCount);

        VkSemaphore imageAvailable(uint32_t frameIndex) const { return mImageAvailable[frameIndex]; }
        VkSemaphore renderFinished(uint32_t imageIndex) const { return mRenderFinished[imageIndex]; }
        VkFence inFlightFence(uint32_t frameIndex) const { return mInFlightFences[frameIndex]; }

    private:
        void destroyImageSemaphores();

        Device& mDevice;
        std::array<VkSemaphore, maxFramesInFlight> mImageAvailable = {};
        std::vector<VkSemaphore> mRenderFinished;
        std::array<VkFence, maxFramesInFlight> mInFlightFences = {};
    };
}

#endif
