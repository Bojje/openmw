#ifndef OPENMW_COMPONENTS_RENDER_FRAME_H
#define OPENMW_COMPONENTS_RENDER_FRAME_H

#include <optional>

#include "texture.hpp"

namespace Render
{
    struct SceneSubmission;
    class FrameStats;

    /// Owns the engine's frame traversal and presentation boundary.
    /// Implementations must initialize and drive exactly one renderer. A
    /// submission-consuming implementation must validate a SceneSubmission
    /// before handing it to backend resources.
    class FrameLifecycle
    {
    public:
        enum class Backend
        {
            Osg,
            Vulkan,
        };

        virtual ~FrameLifecycle() = default;

        virtual Backend backend() const = 0;
        virtual bool renderFrame() = 0;
        virtual bool renderFrame(const SceneSubmission& /*submission*/) { return false; }
        virtual bool consumesSceneSubmission() const = 0;
        virtual bool done() const = 0;
        /// Renderer-neutral profiling sink for shared simulation services.
        /// The active frame owner remains responsible for its implementation.
        virtual FrameStats* stats() const { return nullptr; }
        /// Request termination through the active renderer owner.
        /// A backend may use this for window/input shutdown without exposing
        /// backend-specific viewer or device types to the engine.
        virtual void requestQuit() {}
        /// Request a backend-owned screenshot capture. Backends that expose
        /// captureFrame() but still need engine-level file policy may leave
        /// this as a no-op.
        virtual void requestScreenshot() {}
        /// Notify the active presentation owner that the drawable changed.
        /// Backends that do not need an explicit resize operation may ignore it.
        virtual void resize() {}
        /// Capture the last presented frame in renderer-neutral RGBA8 form.
        /// Backends without capture support report no image.
        virtual std::optional<TextureData> captureFrame() { return std::nullopt; }
        virtual double referenceTime() const = 0;
        virtual unsigned frameNumber() const = 0;
        virtual void advanceFrame(double simulationTime) = 0;
    };
}

#endif
