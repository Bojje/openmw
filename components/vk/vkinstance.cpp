#include "vkinstance.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <SDL_vulkan.h>

#include "vkcommon.hpp"

namespace
{
    VkResult createDebugUtilsMessengerEXT(VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger)
    {
        auto func
            = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (func)
            return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    void destroyDebugUtilsMessengerEXT(
        VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator)
    {
        auto func
            = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (func)
            func(instance, debugMessenger, pAllocator);
    }
}

namespace Vk
{
    Instance::Instance(const std::string& appName, const std::string& engineName, bool enableValidation,
        bool headless)
        : mValidationEnabled(enableValidation)
        , mHeadless(headless)
    {
        if (mValidationEnabled && !checkValidationLayerSupport())
        {
            std::clog << "Vulkan validation layers requested but not available, disabling\n";
            mValidationEnabled = false;
        }

        createInstance(appName, engineName);

        if (mValidationEnabled)
            setupDebugMessenger();
    }

    Instance::~Instance()
    {
        if (mDebugMessenger != VK_NULL_HANDLE)
            destroyDebugUtilsMessengerEXT(mInstance, mDebugMessenger, nullptr);
        if (mInstance != VK_NULL_HANDLE)
            vkDestroyInstance(mInstance, nullptr);
    }

    void Instance::createInstance(const std::string& appName, const std::string& engineName)
    {
        VkApplicationInfo appInfo = {};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = appName.c_str();
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = engineName.c_str();
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        uint32_t loaderVersion = VK_API_VERSION_1_0;
        const auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
        if (enumerateInstanceVersion != nullptr)
        {
            const VkResult result = enumerateInstanceVersion(&loaderVersion);
            if (result != VK_SUCCESS && result != VK_ERROR_INCOMPATIBLE_DRIVER)
                VK_CHECK(result);
        }
        appInfo.apiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);

        auto extensions = getRequiredExtensions();
        if (!checkInstanceExtensionSupport(extensions))
            throw std::runtime_error("Required Vulkan instance extension is unavailable");

        VkInstanceCreateInfo createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo = {};
        if (mValidationEnabled)
        {
            createInfo.enabledLayerCount = 1;
            createInfo.ppEnabledLayerNames = &sValidationLayerName;

            debugCreateInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            debugCreateInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debugCreateInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debugCreateInfo.pfnUserCallback = debugCallback;
            debugCreateInfo.pUserData = this;
            createInfo.pNext = &debugCreateInfo;
        }
        else
        {
            createInfo.enabledLayerCount = 0;
        }

        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &mInstance));

        std::clog << "Vulkan instance created\n";
    }

    void Instance::setupDebugMessenger()
    {
        VkDebugUtilsMessengerCreateInfoEXT createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
        createInfo.pUserData = this;

        VK_CHECK(createDebugUtilsMessengerEXT(mInstance, &createInfo, nullptr, &mDebugMessenger));
    }

    std::vector<const char*> Instance::getRequiredExtensions() const
    {
        if (mHeadless)
            return { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };

        // SDL provides the platform-specific surface extensions.
        unsigned int sdlExtensionCount = 0;
        SDL_Vulkan_GetInstanceExtensions(nullptr, &sdlExtensionCount, nullptr);

        std::vector<const char*> extensions(sdlExtensionCount);
        SDL_Vulkan_GetInstanceExtensions(nullptr, &sdlExtensionCount, extensions.data());

        if (mValidationEnabled)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        return extensions;
    }

    bool Instance::checkInstanceExtensionSupport(const std::vector<const char*>& extensions) const
    {
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> available(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, available.data());

        for (const char* required : extensions)
        {
            const auto found = std::find_if(available.begin(), available.end(), [required](const auto& extension) {
                return std::strcmp(extension.extensionName, required) == 0;
            });
            if (found == available.end())
                return false;
        }
        return true;
    }

    bool Instance::checkValidationLayerSupport() const
    {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const auto& layer : availableLayers)
        {
            if (std::strcmp(layer.layerName, sValidationLayerName) == 0)
                return true;
        }
        return false;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL Instance::debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
        VkDebugUtilsMessageTypeFlagsEXT /*messageType*/,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
    {
        auto* instance = static_cast<Instance*>(pUserData);
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            if (instance != nullptr)
                instance->mValidationErrorCount.fetch_add(1);
            std::clog << "Vulkan validation: " << pCallbackData->pMessage << '\n';
        }
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            std::clog << "Vulkan validation: " << pCallbackData->pMessage << '\n';

        return VK_FALSE;
    }
}
