#ifndef OPENMW_COMPONENTS_VK_VKPLATFORM_H
#define OPENMW_COMPONENTS_VK_VKPLATFORM_H

#include <vector>

#include "vkcommon.hpp"

struct SDL_Window;

namespace Vk
{
    // Surface creation deliberately does not go through SDL_Vulkan_*. The prebuilt SDL2 shipped in
    // openmw-deps is compiled without SDL_VIDEO_VULKAN, so SDL_CreateWindow rejects SDL_WINDOW_VULKAN
    // and SDL_Vulkan_CreateSurface always fails. Creating the surface from the native window handle
    // keeps the Vulkan renderer buildable against the stock dependency bundle.

    // Instance extensions needed to present to a window on this platform.
    std::vector<const char*> getRequiredPlatformInstanceExtensions();

    // Creates a surface for the given window. Throws std::runtime_error on failure.
    VkSurfaceKHR createPlatformSurface(VkInstance instance, SDL_Window* window);

    // Pixel size of the window's drawable area. Replaces SDL_Vulkan_GetDrawableSize.
    void getDrawableSize(SDL_Window* window, int& width, int& height);
}

#endif
