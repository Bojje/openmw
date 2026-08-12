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

#include <components/vk/vkrenderer.hpp>

namespace MWRender
{
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

    struct VulkanFrameLifecycle::Implementation
    {
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
        return mImplementation->renderer->renderFrame();
    }

    bool VulkanFrameLifecycle::renderFrame(const Render::SceneSubmission& submission)
    {
        return mImplementation->renderer->renderFrame(submission);
    }

    bool VulkanFrameLifecycle::done() const
    {
        return mImplementation->renderer->done();
    }

    void VulkanFrameLifecycle::requestQuit()
    {
        mImplementation->renderer->requestQuit();
    }

    double VulkanFrameLifecycle::referenceTime() const
    {
        return mImplementation->renderer->referenceTime();
    }

    unsigned VulkanFrameLifecycle::frameNumber() const
    {
        return mImplementation->renderer->frameNumber();
    }

    void VulkanFrameLifecycle::advanceFrame(double simulationTime)
    {
        mImplementation->renderer->advanceFrame(simulationTime);
    }

    void VulkanFrameLifecycle::resize()
    {
        int width = 0;
        int height = 0;
        SDL_Vulkan_GetDrawableSize(mWindow, &width, &height);
        if (width > 0 && height > 0)
            mImplementation->renderer->resize(static_cast<uint32_t>(width), static_cast<uint32_t>(height));
    }

    std::optional<Render::TextureData> VulkanFrameLifecycle::captureFrame()
    {
        return mImplementation->renderer->captureFrame();
    }

    void VulkanFrameLifecycle::captureScreenshot()
    {
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
    }
}
