#ifndef MWRENDER_SCREENSHOTMANAGER_H
#define MWRENDER_SCREENSHOTMANAGER_H

#include <functional>
#include <osg/ref_ptr>

#include <osgViewer/Viewer>

namespace MWRender
{
    class NotifyDrawCompletedCallback;

    class ScreenshotManager
    {
    public:
        ScreenshotManager(osgViewer::Viewer* viewer, std::function<void()> frameRenderer,
            std::function<void()> frameAdvancer);
        ~ScreenshotManager();

        void screenshot(osg::Image* image, int w, int h);

    private:
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        std::function<void()> mFrameRenderer;
        std::function<void()> mFrameAdvancer;
        osg::ref_ptr<NotifyDrawCompletedCallback> mDrawCompleteCallback;
    };
}

#endif
