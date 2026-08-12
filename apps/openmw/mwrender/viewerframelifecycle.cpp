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

    double ViewerFrameLifecycle::referenceTime() const
    {
        const osg::FrameStamp* frameStamp = mViewer.getFrameStamp();
        return frameStamp ? frameStamp->getReferenceTime() : 0.0;
    }

    unsigned ViewerFrameLifecycle::frameNumber() const
    {
        const osg::FrameStamp* frameStamp = mViewer.getFrameStamp();
        return frameStamp ? frameStamp->getFrameNumber() : 0;
    }

    void ViewerFrameLifecycle::advanceFrame(double simulationTime)
    {
        mViewer.advance(simulationTime);
    }
}
