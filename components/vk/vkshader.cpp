#include "vkshader.hpp"

#include "vkcommon.hpp"
#include "vkdevice.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace Vk
{
    ShaderModule::ShaderModule(Device& device, const std::vector<uint32_t>& spirvCode)
        : mDevice(device.handle())
    {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = spirvCode.size() * sizeof(uint32_t);
        createInfo.pCode = spirvCode.data();

        VK_CHECK(vkCreateShaderModule(mDevice, &createInfo, nullptr, &mModule));
    }

    ShaderModule::~ShaderModule()
    {
        cleanup();
    }

    ShaderModule::ShaderModule(ShaderModule&& other) noexcept
        : mDevice(other.mDevice)
        , mModule(other.mModule)
    {
        other.mModule = VK_NULL_HANDLE;
    }

    ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            mDevice = other.mDevice;
            mModule = other.mModule;
            other.mModule = VK_NULL_HANDLE;
        }
        return *this;
    }

    ShaderModule ShaderModule::fromFile(Device& device, const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::ate | std::ios::binary);
        if (!file.is_open())
            throw std::runtime_error("Failed to open shader file: " + path.string());

        auto fileSize = file.tellg();
        if (fileSize % sizeof(uint32_t) != 0)
            throw std::runtime_error("Shader file size is not aligned to 4 bytes: " + path.string());

        std::vector<uint32_t> code(static_cast<size_t>(fileSize) / sizeof(uint32_t));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(code.data()), fileSize);

        return ShaderModule(device, code);
    }

    VkPipelineShaderStageCreateInfo ShaderModule::stageInfo(VkShaderStageFlagBits stage) const
    {
        VkPipelineShaderStageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage = stage;
        info.module = mModule;
        info.pName = "main";
        return info;
    }

    void ShaderModule::cleanup()
    {
        if (mModule != VK_NULL_HANDLE)
        {
            vkDestroyShaderModule(mDevice, mModule, nullptr);
            mModule = VK_NULL_HANDLE;
        }
    }
}
