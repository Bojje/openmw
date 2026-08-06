#ifndef OPENMW_COMPONENTS_VK_VKINSTANCE_H
#define OPENMW_COMPONENTS_VK_VKINSTANCE_H

#include <atomic>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Instance
    {
    public:
        Instance(const std::string& appName, const std::string& engineName, bool enableValidation,
            bool headless = false);
        ~Instance();

        Instance(const Instance&) = delete;
        Instance& operator=(const Instance&) = delete;

        VkInstance handle() const { return mInstance; }
        bool validationEnabled() const { return mValidationEnabled; }
        uint32_t validationErrorCount() const { return mValidationErrorCount.load(); }

    private:
        void createInstance(const std::string& appName, const std::string& engineName);
        void setupDebugMessenger();
        std::vector<const char*> getRequiredExtensions() const;
        bool checkInstanceExtensionSupport(const std::vector<const char*>& extensions) const;
        bool checkValidationLayerSupport() const;

        static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
            VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
            void* pUserData);

        VkInstance mInstance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT mDebugMessenger = VK_NULL_HANDLE;
        bool mValidationEnabled = false;
        bool mHeadless = false;
        std::atomic<uint32_t> mValidationErrorCount{ 0 };

        static constexpr const char* sValidationLayerName = "VK_LAYER_KHRONOS_validation";
    };
}

#endif
