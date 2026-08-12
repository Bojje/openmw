#ifndef OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H
#define OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H

#include <components/render/frame.hpp>

namespace osgViewer
{
    class Viewer;
}

namespace MWRender
{
    /// OSG bootstrap owner used before the game World and RenderingManager exist.
    /// It keeps viewer traversal out of engine and GUI orchestration code.
    class ViewerFrameLifecycle final : public Render::FrameLifecycle
    {
    public:
        explicit ViewerFrameLifecycle(osgViewer::Viewer& viewer);

        bool renderFrame() override;
        bool consumesSceneSubmission() const override { return false; }
        bool done() const override;
        double referenceTime() const override;
        unsigned frameNumber() const override;
        void advanceFrame(double simulationTime) override;

    private:
        osgViewer::Viewer& mViewer;
    };
}

#endif
