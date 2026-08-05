#ifndef OPENMW_COMPONENTS_VK_VKDESCRIPTORS_H
#define OPENMW_COMPONENTS_VK_VKDESCRIPTORS_H

#include <cstdint>
#include <deque>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;

    class DescriptorSetLayoutBuilder
    {
    public:
        DescriptorSetLayoutBuilder& addBinding(
            uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags, uint32_t count = 1);
        VkDescriptorSetLayout build(Device& device);

    private:
        std::vector<VkDescriptorSetLayoutBinding> mBindings;
    };

    class DescriptorPool
    {
    public:
        DescriptorPool() = default;
        DescriptorPool(Device& device, const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets);
        ~DescriptorPool();

        DescriptorPool(const DescriptorPool&) = delete;
        DescriptorPool& operator=(const DescriptorPool&) = delete;
        DescriptorPool(DescriptorPool&& other) noexcept;
        DescriptorPool& operator=(DescriptorPool&& other) noexcept;

        VkDescriptorSet allocate(VkDescriptorSetLayout layout);
        std::vector<VkDescriptorSet> allocateMultiple(VkDescriptorSetLayout layout, uint32_t count);
        void reset();

    private:
        void cleanup();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkDescriptorPool mPool = VK_NULL_HANDLE;
    };

    class DescriptorWriter
    {
    public:
        DescriptorWriter& writeBuffer(
            uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize size, VkDeviceSize offset = 0);
        DescriptorWriter& writeImage(
            uint32_t binding, VkDescriptorType type, VkImageView view, VkSampler sampler, VkImageLayout layout);
        DescriptorWriter& writeAccelerationStructure(uint32_t binding, VkAccelerationStructureKHR accel);
        void update(Device& device, VkDescriptorSet set);

    private:
        std::deque<VkDescriptorBufferInfo> mBufferInfos;
        std::deque<VkDescriptorImageInfo> mImageInfos;
        std::deque<VkAccelerationStructureKHR> mAccelHandles;
        std::deque<VkWriteDescriptorSetAccelerationStructureKHR> mAccelInfos;
        std::vector<VkWriteDescriptorSet> mWrites;
    };
}

#endif
