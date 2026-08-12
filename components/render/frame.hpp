#ifndef OPENMW_COMPONENTS_RENDER_FRAME_H
#define OPENMW_COMPONENTS_RENDER_FRAME_H

namespace Render
{
    struct SceneSubmission;

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
        /// Request termination through the active renderer owner.
        /// A backend may use this for window/input shutdown without exposing
        /// backend-specific viewer or device types to the engine.
        virtual void requestQuit() {}
        virtual double referenceTime() const = 0;
        virtual unsigned frameNumber() const = 0;
        virtual void advanceFrame(double simulationTime) = 0;
    };
}

#endif
