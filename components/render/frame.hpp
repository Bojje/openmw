#ifndef OPENMW_COMPONENTS_RENDER_FRAME_H
#define OPENMW_COMPONENTS_RENDER_FRAME_H

namespace Render
{
    struct SceneData;
    struct SceneSubmission;

    /// Owns the engine's frame traversal and presentation boundary.
    /// Implementations must initialize and drive exactly one renderer.
    class FrameLifecycle
    {
    public:
        virtual ~FrameLifecycle() = default;

        virtual void renderFrame() = 0;
        virtual void renderFrame(const SceneSubmission& submission) = 0;
        virtual bool consumesSceneSubmission() const = 0;
        virtual void synchronizeScene(SceneData& sceneData) = 0;
        virtual void advanceFrame(double simulationTime) = 0;
    };
}

#endif
