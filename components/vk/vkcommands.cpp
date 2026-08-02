#include "vkcommands.hpp"

#include <exception>
#include <stdexcept>

#include <components/debug/debuglog.hpp>

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
        // submitAndWait never returns while the fence is still in use, so nothing can be waiting on it
        // by the time the pool goes away.
        if (mSubmitFence != VK_NULL_HANDLE)
            vkDestroyFence(mDevice.handle(), mSubmitFence, nullptr);

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

    VkFence CommandPool::submitFence()
    {
        // A single fence for the whole pool instead of one per submit. Every submit that goes through
        // submitAndWait blocks until this has signalled before returning, so two submissions can never
        // be waiting on it at once, and a cell load does not create and destroy thousands of fences.
        // The fence is left signalled after a wait and reset immediately before the next submit.
        if (mSubmitFence == VK_NULL_HANDLE)
        {
            VkFenceCreateInfo fenceInfo = {};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;

            // Created into a local and only stored once it succeeded: a failed vkCreateFence is not
            // required to leave the handle alone, and the destructor must not be given a stale one.
            VkFence fence = VK_NULL_HANDLE;
            VK_CHECK(vkCreateFence(mDevice.handle(), &fenceInfo, nullptr, &fence));
            mSubmitFence = fence;
        }

        return mSubmitFence;
    }

    void CommandPool::freeCommandBuffer(VkCommandBuffer commandBuffer)
    {
        if (commandBuffer != VK_NULL_HANDLE)
            vkFreeCommandBuffers(mDevice.handle(), mPool, 1, &commandBuffer);
    }

    VkCommandBuffer CommandPool::beginRecording()
    {
        VkCommandBuffer commandBuffer = allocate();

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        try
        {
            VK_CHECK(vkBeginCommandBuffer(commandBuffer, &beginInfo));
        }
        catch (...)
        {
            // The buffer was allocated but never submitted, so it is not pending and giving it back is
            // safe. Without this the handle would be lost to the caller and only reclaimed when the
            // pool is destroyed.
            freeCommandBuffer(commandBuffer);
            throw;
        }

        return commandBuffer;
    }

    void CommandPool::submitAndWait(VkCommandBuffer commandBuffer, VkQueue queue)
    {
        // Returns the buffer to the pool on the way out, however this function is left, unless the
        // submission may still be executing: freeing a pending command buffer is undefined behaviour,
        // so in that one case it is knowingly leaked and reclaimed when the pool is destroyed.
        struct BufferGuard
        {
            CommandPool& mPool;
            VkCommandBuffer mCommandBuffer;
            bool mPending = false;

            ~BufferGuard()
            {
                if (!mPending)
                    mPool.freeCommandBuffer(mCommandBuffer);
            }
        } guard{ *this, commandBuffer };

        VK_CHECK(vkEndCommandBuffer(commandBuffer));

        VkFence fence = submitFence();
        VK_CHECK(vkResetFences(mDevice.handle(), 1, &fence));

        VkSubmitInfo submitInfo = {};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        // A fence rather than vkQueueWaitIdle: draining the queue also waits on rendering work that has
        // nothing to do with this transfer, so on a shared graphics queue every staging copy stalled
        // the frame in flight as well. The fence waits for this submission only.
        guard.mPending = true;
        const VkResult submitResult = vkQueueSubmit(queue, 1, &submitInfo, fence);
        if (submitResult != VK_SUCCESS)
        {
            // A rejected submission never starts, so the buffer is reclaimable again -- except after a
            // lost device, where the driver may already have taken part of it.
            guard.mPending = submitResult == VK_ERROR_DEVICE_LOST;
            VK_CHECK(submitResult);
        }

        // No timeout. A transfer or acceleration structure build that never signals means the device is
        // gone, and giving up early would only turn that into a more confusing failure later on.
        VK_CHECK(vkWaitForFences(mDevice.handle(), 1, &fence, VK_TRUE, UINT64_MAX));

        // Signalled, so execution has finished and the buffer is no longer pending.
        guard.mPending = false;
    }

    VkCommandBuffer CommandPool::beginSingleTime()
    {
        return beginRecording();
    }

    void CommandPool::endSingleTime(VkCommandBuffer commandBuffer, VkQueue queue)
    {
        submitAndWait(commandBuffer, queue);
    }

    CommandBatch::CommandBatch(CommandPool& pool, VkQueue queue, VkDeviceSize flushThreshold)
        : mPool(pool)
        , mQueue(queue)
        , mFlushThreshold(flushThreshold)
    {
    }

    CommandBatch::~CommandBatch()
    {
        if (mCommandBuffer == VK_NULL_HANDLE)
            return;

        // A destructor that throws during unwinding terminates the process, so a failure here can only
        // be logged. This path is the safety net for an exception thrown part way through a cell load:
        // it still submits what was recorded and still returns the command buffer to the pool. Callers
        // that need to react to a failed upload call flush() at the end of the scope, where the
        // exception can escape normally, which also leaves nothing for this to do.
        try
        {
            flush();
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Vulkan: command batch submit failed while unwinding: " << e.what();
        }
        catch (...)
        {
            Log(Debug::Error) << "Vulkan: command batch submit failed while unwinding";
        }
    }

    VkCommandBuffer CommandBatch::handle()
    {
        // Opened lazily so that flushing at the end of a loop does not leave an empty command buffer
        // behind for the destructor to submit and wait on.
        if (mCommandBuffer == VK_NULL_HANDLE)
            mCommandBuffer = mPool.beginRecording();

        return mCommandBuffer;
    }

    void CommandBatch::flush()
    {
        if (mCommandBuffer == VK_NULL_HANDLE)
            return;

        // Cleared before the submit so that a throwing submit cannot leave the destructor trying to
        // submit the same buffer again. submitAndWait owns the buffer from this point: it frees it on
        // success and on every failure where the buffer never became pending.
        VkCommandBuffer commandBuffer = mCommandBuffer;
        mCommandBuffer = VK_NULL_HANDLE;
        mPendingBytes = 0;

        mPool.submitAndWait(commandBuffer, mQueue);
    }

    bool CommandBatch::addPendingBytes(VkDeviceSize bytes)
    {
        mPendingBytes += bytes;

        if (mFlushThreshold == 0 || mPendingBytes < mFlushThreshold)
            return false;

        flush();
        return true;
    }
}
