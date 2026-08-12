#include "vulkanframelifecycle.hpp"

#include <stdexcept>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

#ifdef OPENMW_USE_VULKAN
#include <components/vk/vkrenderer.hpp>
#endif

namespace MWRender
{
    struct VulkanFrameLifecycle::Implementation
    {
#ifdef OPENMW_USE_VULKAN
        explicit Implementation(SDL_Window* window, const std::filesystem::path& shaderDirectory)
            : renderer(std::make_unique<Vk::Renderer>(window, true, Vk::Renderer::SurfaceMode::Window,
                  static_cast<uint32_t>(Settings::video().mResolutionX),
                  static_cast<uint32_t>(Settings::video().mResolutionY)))
        {
            if (!renderer->loadShadersAndCreatePipelines(shaderDirectory.string()))
                throw std::runtime_error("OpenMW Vulkan shaders could not be loaded");
            Log(Debug::Info) << "Using the experimental Vulkan frame lifecycle";
        }

        std::unique_ptr<Vk::Renderer> renderer;
#else
        Implementation(SDL_Window*, const std::filesystem::path&)
        {
            throw std::logic_error("The Vulkan frame lifecycle requires OPENMW_USE_VULKAN");
        }
#endif
    };

    VulkanFrameLifecycle::VulkanFrameLifecycle(SDL_Window* window, const std::filesystem::path& shaderDirectory)
        : mWindow(window)
        , mImplementation(std::make_unique<Implementation>(window, shaderDirectory))
    {
        if (!mWindow)
            throw std::invalid_argument("Vulkan frame lifecycle requires an SDL window");
    }

    VulkanFrameLifecycle::~VulkanFrameLifecycle() = default;

    bool VulkanFrameLifecycle::renderFrame()
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->renderFrame();
#else
        return false;
#endif
    }

    bool VulkanFrameLifecycle::renderFrame(const Render::SceneSubmission& submission)
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->renderFrame(submission);
#else
        static_cast<void>(submission);
        return false;
#endif
    }

    bool VulkanFrameLifecycle::done() const
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->done();
#else
        return true;
#endif
    }

    void VulkanFrameLifecycle::requestQuit()
    {
#ifdef OPENMW_USE_VULKAN
        mImplementation->renderer->requestQuit();
#endif
    }

    double VulkanFrameLifecycle::referenceTime() const
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->referenceTime();
#else
        return 0.0;
#endif
    }

    unsigned VulkanFrameLifecycle::frameNumber() const
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->frameNumber();
#else
        return 0;
#endif
    }

    void VulkanFrameLifecycle::advanceFrame(double simulationTime)
    {
#ifdef OPENMW_USE_VULKAN
        mImplementation->renderer->advanceFrame(simulationTime);
#else
        static_cast<void>(simulationTime);
#endif
    }

    void VulkanFrameLifecycle::resize()
    {
#ifdef OPENMW_USE_VULKAN
        int width = 0;
        int height = 0;
        SDL_Vulkan_GetDrawableSize(mWindow, &width, &height);
        if (width > 0 && height > 0)
            mImplementation->renderer->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
#endif
    }

    std::optional<Render::TextureData> VulkanFrameLifecycle::captureFrame()
    {
#ifdef OPENMW_USE_VULKAN
        return mImplementation->renderer->captureFrame();
#else
        return std::nullopt;
#endif
    }
}
