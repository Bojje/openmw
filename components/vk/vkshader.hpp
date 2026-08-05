#ifndef OPENMW_COMPONENTS_VK_VKSHADER_H
#define OPENMW_COMPONENTS_VK_VKSHADER_H

#include <cstdint>
#include <filesystem>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;

    class ShaderModule
    {
    public:
        ShaderModule() = default;
        ShaderModule(Device& device, const std::vector<uint32_t>& spirvCode);
        ~ShaderModule();

        ShaderModule(const ShaderModule&) = delete;
        ShaderModule& operator=(const ShaderModule&) = delete;
        ShaderModule(ShaderModule&& other) noexcept;
        ShaderModule& operator=(ShaderModule&& other) noexcept;

        static ShaderModule fromFile(Device& device, const std::filesystem::path& path);

        VkShaderModule handle() const { return mModule; }
        VkPipelineShaderStageCreateInfo stageInfo(VkShaderStageFlagBits stage) const;

    private:
        void cleanup();

        VkDevice mDevice = VK_NULL_HANDLE;
        VkShaderModule mModule = VK_NULL_HANDLE;
    };
}

#endif
