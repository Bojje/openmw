#include "vkinstance.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>

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
    Instance::Instance(const std::string& appName, const std::string& engineName, bool enableValidation)
        : mValidationEnabled(enableValidation)
    {
        if (mValidationEnabled && !checkValidationLayerSupport())
        {
            Log(Debug::Warning) << "Vulkan validation layers requested but not available, disabling";
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

    Instance::Instance(Instance&& other) noexcept
        : mInstance(other.mInstance)
        , mDebugMessenger(other.mDebugMessenger)
        , mValidationEnabled(other.mValidationEnabled)
    {
        other.mInstance = VK_NULL_HANDLE;
        other.mDebugMessenger = VK_NULL_HANDLE;
    }

    Instance& Instance::operator=(Instance&& other) noexcept
    {
        if (this != &other)
        {
            if (mDebugMessenger != VK_NULL_HANDLE)
                destroyDebugUtilsMessengerEXT(mInstance, mDebugMessenger, nullptr);
            if (mInstance != VK_NULL_HANDLE)
                vkDestroyInstance(mInstance, nullptr);

            mInstance = other.mInstance;
            mDebugMessenger = other.mDebugMessenger;
            mValidationEnabled = other.mValidationEnabled;

            other.mInstance = VK_NULL_HANDLE;
            other.mDebugMessenger = VK_NULL_HANDLE;
        }
        return *this;
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
            VK_CHECK(enumerateInstanceVersion(&loaderVersion));
        appInfo.apiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);

        auto extensions = getRequiredExtensions();

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
            createInfo.pNext = &debugCreateInfo;
        }
        else
        {
            createInfo.enabledLayerCount = 0;
        }

        VK_CHECK(vkCreateInstance(&createInfo, nullptr, &mInstance));

        Log(Debug::Info) << "Vulkan instance created";
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

        VK_CHECK(createDebugUtilsMessengerEXT(mInstance, &createInfo, nullptr, &mDebugMessenger));
    }

    std::vector<const char*> Instance::getRequiredExtensions() const
    {
        // SDL provides the platform-specific surface extensions
        unsigned int sdlExtensionCount = 0;
        SDL_Vulkan_GetInstanceExtensions(nullptr, &sdlExtensionCount, nullptr);

        std::vector<const char*> extensions(sdlExtensionCount);
        SDL_Vulkan_GetInstanceExtensions(nullptr, &sdlExtensionCount, extensions.data());

        if (mValidationEnabled)
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

        return extensions;
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
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* /*pUserData*/)
    {
        if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            Log(Debug::Error) << "Vulkan validation: " << pCallbackData->pMessage;
        else if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            Log(Debug::Warning) << "Vulkan validation: " << pCallbackData->pMessage;

        return VK_FALSE;
    }
}
