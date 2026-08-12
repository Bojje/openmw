#include "vulkanframelifecycle.hpp"

#include <chrono>
#include <fstream>
#include <system_error>
#include <stdexcept>
#include <utility>
#include <vector>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>
#include <components/render/imagewriter.hpp>
#ifdef OPENMW_NEUTRAL_JPEG
#include <components/render/jpeg.hpp>
#endif
#ifdef OPENMW_NEUTRAL_PNG
#include <components/render/png.hpp>
#endif
#include <components/settings/values.hpp>

#ifdef OPENMW_USE_VULKAN
#include <components/vk/vkrenderer.hpp>
#endif

namespace MWRender
{
#ifdef OPENMW_USE_VULKAN
    namespace
    {
        bool writeScreenshot(const Render::TextureData& image, const std::filesystem::path& path)
        {
            if (!image.valid())
                return false;
            if (path.extension() == ".jpg")
            {
#ifdef OPENMW_NEUTRAL_JPEG
                std::vector<char> encoded;
                if (!Render::writeJpeg(image, encoded))
                    return false;
                std::ofstream output(path, std::ios::binary);
                if (!output)
                    return false;
                output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                return output.good();
#else
                return false;
#endif
            }
            if (path.extension() == ".png")
            {
#ifdef OPENMW_NEUTRAL_PNG
                std::vector<char> encoded;
                if (!Render::writePng(image, encoded))
                    return false;
                std::ofstream output(path, std::ios::binary);
                if (!output)
                    return false;
                output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
                return output.good();
#else
                return false;
#endif
            }
            if (path.extension() == ".tga")
                return Render::writeTga(image, path);
            return Render::writePpm(image, path);
        }
    }
#endif

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

    VulkanFrameLifecycle::VulkanFrameLifecycle(SDL_Window* window, const std::filesystem::path& shaderDirectory,
        const std::filesystem::path& screenshotPath, std::string screenshotFormat)
        : mWindow(window)
        , mScreenshotPath(screenshotPath)
        , mScreenshotFormat(std::move(screenshotFormat))
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

    void VulkanFrameLifecycle::captureScreenshot()
    {
#ifdef OPENMW_USE_VULKAN
        const std::optional<Render::TextureData> image = captureFrame();
        if (!image)
        {
            Log(Debug::Warning) << "Vulkan screenshot requested before a frame was presented";
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(mScreenshotPath, error);
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool jpeg = mScreenshotFormat == "jpg";
        const bool png = mScreenshotFormat == "png";
        const bool tga = mScreenshotFormat == "tga";
        const std::filesystem::path path = mScreenshotPath
            / ("openmw-vulkan-" + std::to_string(stamp) + (jpeg ? ".jpg" : png ? ".png" : tga ? ".tga" : ".ppm"));
        if (!writeScreenshot(*image, path))
            Log(Debug::Warning) << "Failed to write Vulkan screenshot " << path;
        else
            Log(Debug::Info) << "Vulkan screenshot written to " << path;
#endif
    }
}
