#include "vkdescriptors.hpp"

#include "vkcommon.hpp"
#include "vkdevice.hpp"

#include <utility>

namespace Vk
{
    DescriptorSetLayoutBuilder& DescriptorSetLayoutBuilder::addBinding(
        uint32_t binding, VkDescriptorType type, VkShaderStageFlags stageFlags, uint32_t count)
    {
        VkDescriptorSetLayoutBinding layoutBinding{};
        layoutBinding.binding = binding;
        layoutBinding.descriptorType = type;
        layoutBinding.descriptorCount = count;
        layoutBinding.stageFlags = stageFlags;
        layoutBinding.pImmutableSamplers = nullptr;
        mBindings.push_back(layoutBinding);
        return *this;
    }

    VkDescriptorSetLayout DescriptorSetLayoutBuilder::build(Device& device)
    {
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(mBindings.size());
        layoutInfo.pBindings = mBindings.data();

        VkDescriptorSetLayout layout;
        VK_CHECK(vkCreateDescriptorSetLayout(device.handle(), &layoutInfo, nullptr, &layout));
        return layout;
    }

    DescriptorPool::DescriptorPool(
        Device& device, const std::vector<VkDescriptorPoolSize>& poolSizes, uint32_t maxSets)
        : mDevice(device.handle())
    {
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        poolInfo.pPoolSizes = poolSizes.data();
        poolInfo.maxSets = maxSets;

        VK_CHECK(vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mPool));
    }

    DescriptorPool::~DescriptorPool()
    {
        cleanup();
    }

    DescriptorPool::DescriptorPool(DescriptorPool&& other) noexcept
        : mDevice(other.mDevice)
        , mPool(other.mPool)
    {
        other.mPool = VK_NULL_HANDLE;
    }

    DescriptorPool& DescriptorPool::operator=(DescriptorPool&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            mDevice = other.mDevice;
            mPool = other.mPool;
            other.mPool = VK_NULL_HANDLE;
        }
        return *this;
    }

    VkDescriptorSet DescriptorPool::allocate(VkDescriptorSetLayout layout)
    {
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = mPool;
        allocInfo.descriptorSetCount = 1;
        allocInfo.pSetLayouts = &layout;

        VkDescriptorSet descriptorSet;
        VK_CHECK(vkAllocateDescriptorSets(mDevice, &allocInfo, &descriptorSet));
        return descriptorSet;
    }

    std::vector<VkDescriptorSet> DescriptorPool::allocateMultiple(VkDescriptorSetLayout layout, uint32_t count)
    {
        std::vector<VkDescriptorSetLayout> layouts(count, layout);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = mPool;
        allocInfo.descriptorSetCount = count;
        allocInfo.pSetLayouts = layouts.data();

        std::vector<VkDescriptorSet> descriptorSets(count);
        VK_CHECK(vkAllocateDescriptorSets(mDevice, &allocInfo, descriptorSets.data()));
        return descriptorSets;
    }

    void DescriptorPool::reset()
    {
        VK_CHECK(vkResetDescriptorPool(mDevice, mPool, 0));
    }

    void DescriptorPool::cleanup()
    {
        if (mPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice, mPool, nullptr);
            mPool = VK_NULL_HANDLE;
        }
    }

    DescriptorWriter& DescriptorWriter::writeBuffer(
        uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize size, VkDeviceSize offset)
    {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = buffer;
        bufferInfo.offset = offset;
        bufferInfo.range = size;
        mBufferInfos.push_back(bufferInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pBufferInfo = &mBufferInfos.back();
        mWrites.push_back(write);

        return *this;
    }

    DescriptorWriter& DescriptorWriter::writeImage(
        uint32_t binding, VkDescriptorType type, VkImageView view, VkSampler sampler, VkImageLayout layout)
    {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = view;
        imageInfo.sampler = sampler;
        imageInfo.imageLayout = layout;
        mImageInfos.push_back(imageInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = type;
        write.descriptorCount = 1;
        write.pImageInfo = &mImageInfos.back();
        mWrites.push_back(write);

        return *this;
    }

    DescriptorWriter& DescriptorWriter::writeAccelerationStructure(
        uint32_t binding, VkAccelerationStructureKHR accel)
    {
        mAccelHandles.push_back(accel);

        VkWriteDescriptorSetAccelerationStructureKHR accelInfo{};
        accelInfo.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
        accelInfo.accelerationStructureCount = 1;
        accelInfo.pAccelerationStructures = &mAccelHandles.back();
        mAccelInfos.push_back(accelInfo);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.pNext = &mAccelInfos.back();
        write.dstBinding = binding;
        write.dstArrayElement = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        write.descriptorCount = 1;
        mWrites.push_back(write);

        return *this;
    }

    void DescriptorWriter::update(Device& device, VkDescriptorSet set)
    {
        for (auto& write : mWrites)
            write.dstSet = set;

        vkUpdateDescriptorSets(
            device.handle(), static_cast<uint32_t>(mWrites.size()), mWrites.data(), 0, nullptr);
    }
}
