#ifndef OPENMW_MWRENDER_VULKAN_FRAME_LIFECYCLE_H
#define OPENMW_MWRENDER_VULKAN_FRAME_LIFECYCLE_H

#include <filesystem>
#include <memory>
#include <optional>

#include <components/render/frame.hpp>
#include <components/render/texture.hpp>

struct SDL_Window;

namespace MWRender
{
    /// Owns the Vulkan renderer used by the experimental full-game path.
    /// The SDL window remains owned by Engine so both frame owners follow the
    /// same lifetime contract.
    class VulkanFrameLifecycle final : public Render::FrameLifecycle
    {
    public:
        VulkanFrameLifecycle(SDL_Window* window, const std::filesystem::path& shaderDirectory);
        ~VulkanFrameLifecycle() override;

        VulkanFrameLifecycle(const VulkanFrameLifecycle&) = delete;
        VulkanFrameLifecycle& operator=(const VulkanFrameLifecycle&) = delete;

        SDL_Window* window() const { return mWindow; }

        Render::FrameLifecycle::Backend backend() const override { return Render::FrameLifecycle::Backend::Vulkan; }
        bool renderFrame() override;
        bool renderFrame(const Render::SceneSubmission& submission) override;
        bool consumesSceneSubmission() const override { return true; }
        bool done() const override;
        void requestQuit() override;
        double referenceTime() const override;
        unsigned frameNumber() const override;
        void advanceFrame(double simulationTime) override;

        void resize();
        std::optional<Render::TextureData> captureFrame();

    private:
        struct Implementation;
        SDL_Window* mWindow;
        std::unique_ptr<Implementation> mImplementation;
    };
}

#endif
