#include "viewerframelifecycle.hpp"

#include <utility>

#include <osgViewer/Viewer>

namespace MWRender
{
    ViewerFrameLifecycle::ViewerFrameLifecycle(osgViewer::Viewer& viewer,
        std::function<void(Render::SceneData&)> synchronizeScene)
        : mViewer(viewer)
        , mSynchronizeScene(std::move(synchronizeScene))
    {
    }

    bool ViewerFrameLifecycle::renderFrame()
    {
        mViewer.eventTraversal();
        mViewer.updateTraversal();
        mViewer.renderingTraversals();
        return true;
    }

    bool ViewerFrameLifecycle::renderFrame(const Render::SceneSubmission& /*submission*/)
    {
        return renderFrame();
    }

    void ViewerFrameLifecycle::synchronizeScene(Render::SceneData& sceneData)
    {
        if (mSynchronizeScene)
            mSynchronizeScene(sceneData);
    }

    void ViewerFrameLifecycle::advanceFrame(double simulationTime)
    {
        mViewer.advance(simulationTime);
    }
}
