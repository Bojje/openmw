#ifndef OPENMW_COMPONENTS_VK_VKINSTANCE_H
#define OPENMW_COMPONENTS_VK_VKINSTANCE_H

#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Instance
    {
    public:
        Instance(const std::string& appName, const std::string& engineName, bool enableValidation);
        ~Instance();

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;
        Instance(Instance&& other) noexcept;
        Instance& operator=(Instance&& other) noexcept;

        VkInstance handle() const { return mInstance; }
        bool validationEnabled() const { return mValidationEnabled; }

    private:
        void createInstance(const std::string& appName, const std::string& engineName);
        void setupDebugMessenger();
        std::vector<const char*> getRequiredExtensions() const;
        bool checkValidationLayerSupport() const;

        static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void* pUserData);

        VkInstance mInstance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
        bool mValidationEnabled = false;

        static constexpr const char* sValidationLayerName = "VK_LAYER_KHRONOS_validation";
    };
}

#endif
