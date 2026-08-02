#ifndef OPENMW_COMPONENTS_VK_VKCOMMANDS_H
#define OPENMW_COMPONENTS_VK_VKCOMMANDS_H

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;
    class CommandBatch;

    class CommandPool
    {
    public:
        CommandPool(Device& device, uint32_t queueFamilyIndex);
        ~CommandPool();

        CommandPool(const CommandPool&) = delete;
        CommandPool& operator=(const CommandPool&) = delete;

        VkCommandBuffer allocate();
        std::vector<VkCommandBuffer> allocateMultiple(uint32_t count);

        // One command buffer, one submit and one blocking wait per operation. Fine for a one-off
        // transfer at startup; for anything that happens in a loop use CommandBatch instead, which
        // shares a buffer and a submit across the whole loop.
        VkCommandBuffer beginSingleTime();
        void endSingleTime(VkCommandBuffer commandBuffer, VkQueue queue);

        VkCommandPool handle() const { return mPool; }

    private:
        friend class CommandBatch;

        // beginSingleTime/endSingleTime and CommandBatch are both written in terms of these, so there
        // is one implementation of "start recording a one-time buffer" and one of "submit it, block
        // until the GPU is done, hand it back to the pool".
        VkCommandBuffer beginRecording();
        void submitAndWait(VkCommandBuffer commandBuffer, VkQueue queue);
        void freeCommandBuffer(VkCommandBuffer commandBuffer);
        VkFence submitFence();

        Device& mDevice;
        VkCommandPool mPool = VK_NULL_HANDLE;

        // One fence reused by every blocking submit. Created on first use, destroyed with the pool.
        VkFence mSubmitFence = VK_NULL_HANDLE;
    };

    // Records any number of one-time operations into a single command buffer and pays for one submit
    // and one wait instead of one per operation. A cell load runs roughly 4,600 staging copies, BLAS
    // builds and texture uploads; done one at a time that is 4,600 full stalls on the main thread.
    //
    // Usage:
    //
    //     Vk::CommandBatch batch(commandPool, device.graphicsQueue());
    //     for (const auto& mesh : meshes)
    //     {
    //         vkCmdCopyBuffer(batch.handle(), staging, target, 1, &region);
    //         batch.addPendingBytes(region.size);   // may flush, see below
    //     }
    //     batch.flush();   // optional: submits here rather than in the destructor, so a failed
    //                      // upload throws where the caller can still do something about it
    //
    // STAGING BUFFER LIFETIME. This is the sharp edge of batching. Recording a copy does not perform
    // it; the GPU only reads the source when the batch is submitted. Every staging buffer, source
    // image and BLAS scratch allocation recorded into a batch must therefore stay alive until the
    // submit that consumes it has completed. With beginSingleTime/endSingleTime a staging buffer could
    // be destroyed on the next line because the submit had already been waited on; inside a batch that
    // is a use-after-free, and the validation layers do not reliably catch it. The batch owns nothing
    // and cannot check this for you. flush() and the destructor both block until the GPU has finished,
    // so anything recorded before they return is safe to destroy after them.
    //
    // THREADING. VkCommandPool and VkQueue are externally synchronized objects, and this batch takes
    // no locks because the renderer is single threaded today. Moving cell loading onto worker threads
    // means giving each thread its own CommandPool (and so its own batches) and synchronizing access
    // to the queue at the point of submission; a mutex inside this class would not be enough, because
    // recording goes through the raw VkCommandBuffer that handle() returns.
    class CommandBatch
    {
    public:
        // Flushing at 64 MiB of tracked memory keeps peak staging allocation bounded and starts the
        // GPU working part way through a long cell load instead of leaving everything until the end.
        static constexpr VkDeviceSize sDefaultFlushThreshold = 64ull * 1024 * 1024;

        // No Vulkan work happens here; the command buffer is allocated on the first handle() call, so
        // a batch that ends up recording nothing costs nothing. Pass a threshold of 0 to disable
        // automatic flushing entirely.
        CommandBatch(CommandPool& pool, VkQueue queue, VkDeviceSize flushThreshold = sDefaultFlushThreshold);
        ~CommandBatch();

        CommandBatch(const CommandBatch&) = delete;
        CommandBatch& operator=(const CommandBatch&) = delete;

        // The buffer to record into, for use with vkCmdCopyBuffer, vkCmdPipelineBarrier,
        // vkCmdBuildAccelerationStructuresKHR and the rest unchanged. A flush retires the current
        // buffer and the next call to handle() opens a new one, so never cache the returned handle
        // across a flush.
        VkCommandBuffer handle();

        // Submits everything recorded so far and blocks until the GPU has finished it. The batch stays
        // usable afterwards: keep recording and the next flush (or the destructor) submits the rest.
        // Call this explicitly at the end of a scope when submission errors need to be handled.
        void flush();

        // Reports memory that has to stay alive until the next submit, so the batch can flush itself
        // before too much of it piles up. Returns true if that flush happened, meaning everything
        // recorded up to this point has completed and its staging buffers can now be released.
        bool addPendingBytes(VkDeviceSize bytes);

        VkDeviceSize pendingBytes() const { return mPendingBytes; }

        // True while there is recorded work that has not been submitted yet.
        bool recording() const { return mCommandBuffer != VK_NULL_HANDLE; }

    private:
        CommandPool& mPool;
        VkQueue mQueue = VK_NULL_HANDLE;
        VkCommandBuffer mCommandBuffer = VK_NULL_HANDLE;
        VkDeviceSize mFlushThreshold = 0;
        VkDeviceSize mPendingBytes = 0;
    };
}

#endif
