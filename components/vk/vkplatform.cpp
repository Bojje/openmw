// vulkan.h only declares the platform surface entry points and structs whose VK_USE_PLATFORM_*
// macro is defined before it is included, and vkcommon.hpp (pulled in by vkplatform.hpp) is what
// includes vulkan.h. The defines therefore have to be set here, in this translation unit, rather
// than left to the build system: a global define would leak into every other TU that includes
// vkcommon.hpp, and relying on one being passed on the command line would silently degrade into
// "vkCreateXlibSurfaceKHR was not declared" if it ever went missing.
#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)                        \
    || defined(__DragonFly__)
// Both, not one or the other. The Steam Deck runs an X11 session in desktop mode and a Wayland
// compositor in Game Mode, and which one a given launch lands in is not knowable at build time, so
// the binary has to carry both code paths and choose from SDL_SysWMinfo::subsystem at runtime.
#define OPENMW_VK_PLATFORM_UNIX
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif

#include "vkplatform.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include <SDL_syswm.h>
#include <SDL_video.h>

// The x11/wl members of the SDL_SysWMinfo union only exist when the SDL these headers came from was
// configured with the matching video driver, so fail loudly at build time instead of emitting a
// confusing "has no member named 'x11'" further down.
#if defined(OPENMW_VK_PLATFORM_UNIX) && !defined(SDL_VIDEO_DRIVER_X11) && !defined(SDL_VIDEO_DRIVER_WAYLAND)
#error "SDL was built without both the X11 and the Wayland video driver, so SDL_SysWMinfo exposes no native window handle to create a Vulkan surface from"
#endif

namespace
{
#if defined(OPENMW_VK_PLATFORM_UNIX)
    std::vector<VkExtensionProperties> enumerateInstanceExtensions()
    {
        uint32_t count = 0;
        VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr));

        std::vector<VkExtensionProperties> available(count);
        if (count > 0)
            VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data()));

        return available;
    }

    bool isExtensionSupported(const std::vector<VkExtensionProperties>& available, const char* name)
    {
        for (const VkExtensionProperties& extension : available)
        {
            if (std::strcmp(extension.extensionName, name) == 0)
                return true;
        }
        return false;
    }
#endif
}

namespace Vk
{
    std::vector<const char*> getRequiredPlatformInstanceExtensions()
    {
        std::vector<const char*> extensions{ VK_KHR_SURFACE_EXTENSION_NAME };
#if defined(_WIN32)
        extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#elif defined(OPENMW_VK_PLATFORM_UNIX)
        // Ask for whichever of the two surface extensions the loader actually advertises rather than
        // hardcoding one. vkCreateInstance fails outright with VK_ERROR_EXTENSION_NOT_PRESENT if any
        // requested extension is missing, so hardcoding both would break a machine whose loader only
        // has one of them (an X11-only install, or a headless/Wayland-only one), and hardcoding one
        // would break the other session type. The window's actual subsystem is not known here - the
        // instance is created before the surface - so both are requested when both are present and
        // createPlatformSurface picks the matching one later.
        const std::vector<VkExtensionProperties> available = enumerateInstanceExtensions();

        if (isExtensionSupported(available, VK_KHR_XLIB_SURFACE_EXTENSION_NAME))
            extensions.push_back(VK_KHR_XLIB_SURFACE_EXTENSION_NAME);
        if (isExtensionSupported(available, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME))
            extensions.push_back(VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME);

        if (extensions.size() == 1)
            throw std::runtime_error("The Vulkan instance supports neither " VK_KHR_XLIB_SURFACE_EXTENSION_NAME
                " nor " VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME ", cannot present to a window");
#else
#error "Vk::getRequiredPlatformInstanceExtensions is only implemented for Win32, Xlib and Wayland"
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
#elif defined(OPENMW_VK_PLATFORM_UNIX)
        // Runtime, not compile time: the same build has to work in a desktop X11 session and in a
        // Wayland one, and SDL decides which video driver it used when the window was created.
        switch (wmInfo.subsystem)
        {
#if defined(SDL_VIDEO_DRIVER_X11)
            case SDL_SYSWM_X11:
            {
                VkXlibSurfaceCreateInfoKHR createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
                createInfo.dpy = wmInfo.info.x11.display;
                createInfo.window = wmInfo.info.x11.window;

                VkSurfaceKHR surface = VK_NULL_HANDLE;
                VK_CHECK(vkCreateXlibSurfaceKHR(instance, &createInfo, nullptr, &surface));
                return surface;
            }
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
            case SDL_SYSWM_WAYLAND:
            {
                VkWaylandSurfaceCreateInfoKHR createInfo = {};
                createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
                createInfo.display = wmInfo.info.wl.display;
                createInfo.surface = wmInfo.info.wl.surface;

                VkSurfaceKHR surface = VK_NULL_HANDLE;
                VK_CHECK(vkCreateWaylandSurfaceKHR(instance, &createInfo, nullptr, &surface));
                return surface;
            }
#endif
            default:
                // Covers SDL_SYSWM_KMSDRM and friends, and an X11 or Wayland window whose driver was
                // compiled out of this SDL. Naming the value makes a mismatched SDL build obvious.
                throw std::runtime_error("Unsupported SDL window subsystem "
                    + std::to_string(static_cast<int>(wmInfo.subsystem))
                    + ", expected SDL_SYSWM_X11 or SDL_SYSWM_WAYLAND");
        }
#else
#error "Vk::createPlatformSurface is only implemented for Win32, Xlib and Wayland"
#endif
    }

    void getDrawableSize(SDL_Window* window, int& width, int& height)
    {
#if defined(OPENMW_VK_PLATFORM_UNIX) && SDL_VERSION_ATLEAST(2, 26, 0)
        // Under Wayland with output scaling the window size SDL reports is in logical units, which
        // is not the pixel size the swapchain has to match. SDL_GetWindowSizeInPixels gives the
        // pixel size without going through SDL_Vulkan_GetDrawableSize, which is unusable here for
        // the reason given in vkplatform.hpp. On X11 it returns the window size unchanged.
        SDL_GetWindowSizeInPixels(window, &width, &height);
#else
        // On Windows SDL reports window size in pixels already, so this matches what
        // SDL_Vulkan_GetDrawableSize would have returned.
        SDL_GetWindowSize(window, &width, &height);
#endif
    }
}
