#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#endif

#include "vkplatform.hpp"

#include <stdexcept>
#include <string>

#include <SDL_syswm.h>
#include <SDL_video.h>

namespace Vk
{
    std::vector<const char*> getRequiredPlatformInstanceExtensions()
    {
        std::vector<const char*> extensions{ VK_KHR_SURFACE_EXTENSION_NAME };
#if defined(_WIN32)
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
#error "Vk::getRequiredPlatformInstanceExtensions is only implemented for Win32"
#endif
        return extensions;
    }

    VkSurfaceKHR createPlatformSurface(VkInstance instance, SDL_Window* window)
    {
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (!SDL_GetWindowWMInfo(window, &wmInfo))
            throw std::runtime_error(std::string("SDL_GetWindowWMInfo failed: ") + SDL_GetError());

#if defined(_WIN32)
        if (wmInfo.subsystem != SDL_SYSWM_WINDOWS)
            throw std::runtime_error("Unexpected SDL window subsystem, expected SDL_SYSWM_WINDOWS");

        VkWin32SurfaceCreateInfoKHR createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
        createInfo.hinstance = wmInfo.info.win.hinstance;
        createInfo.hwnd = wmInfo.info.win.window;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VK_CHECK(vkCreateWin32SurfaceKHR(instance, &createInfo, nullptr, &surface));
        return surface;
#else
#error "Vk::createPlatformSurface is only implemented for Win32"
#endif
    }

    void getDrawableSize(SDL_Window* window, int& width, int& height)
    {
        // On Windows SDL reports window size in pixels already, so this matches what
        // SDL_Vulkan_GetDrawableSize would have returned.
        SDL_GetWindowSize(window, &width, &height);
    }
}
