#ifndef OPENMW_COMPONENTS_RENDER_FRAME_H
#define OPENMW_COMPONENTS_RENDER_FRAME_H

namespace Render
{
    /// Owns the engine's frame traversal and presentation boundary.
    /// Implementations must initialize and drive exactly one renderer.
    class FrameLifecycle
    {
    public:
        virtual ~FrameLifecycle() = default;

        virtual void renderFrame() = 0;
        virtual void advanceFrame(double simulationTime) = 0;
    };
}

#endif
