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
        virtual ~FrameLifecycle() = default;

        virtual bool renderFrame() = 0;
        virtual bool renderFrame(const SceneSubmission& /*submission*/) { return false; }
        virtual bool consumesSceneSubmission() const = 0;
        virtual void advanceFrame(double simulationTime) = 0;
    };
}

#endif
