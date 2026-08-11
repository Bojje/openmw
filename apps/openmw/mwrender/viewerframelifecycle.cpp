#include "viewerframelifecycle.hpp"

#include <osgViewer/Viewer>

namespace MWRender
{
    ViewerFrameLifecycle::ViewerFrameLifecycle(osgViewer::Viewer& viewer)
        : mViewer(viewer)
    {
    }

    bool ViewerFrameLifecycle::renderFrame()
    {
        mViewer.eventTraversal();
        mViewer.updateTraversal();
        mViewer.renderingTraversals();
        return true;
    }

    void ViewerFrameLifecycle::advanceFrame(double simulationTime)
    {
        mViewer.advance(simulationTime);
    }
}
