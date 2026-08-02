#ifndef OPENMW_COMPONENTS_SCENEUTIL_SCREENCAPTURE_H
#define OPENMW_COMPONENTS_SCENEUTIL_SCREENCAPTURE_H

#include <components/misc/guarded.hpp>

#include <osg/ref_ptr>
#include <osgViewer/ViewerEventHandlers>

#include <filesystem>
#include <string>
#include <vector>

namespace osg
{
    class Image;
}

namespace SceneUtil
{
    class WorkQueue;
    class WorkItem;

    /// Writes \a image into \a screenshotPath as the first free "<namePrefix>NNN.<format>", and
    /// returns that file name. \a namePrefix exists so a second renderer can write alongside the
    /// first without the two taking turns at the same numbering -- which backend a file came from
    /// then reads off its name rather than off the order they happened to be written in.
    std::filesystem::path writeScreenshotToFile(const std::filesystem::path& screenshotPath,
        const std::string& screenshotFormat, const osg::Image& image,
        const std::string& namePrefix = "screenshot");

    class WriteScreenshotToFileOperation : public osgViewer::ScreenCaptureHandler::CaptureOperation
    {
    public:
        WriteScreenshotToFileOperation(const std::filesystem::path& screenshotPath, const std::string& screenshotFormat,
            std::function<void(std::string)> callback);

        void operator()(const osg::Image& image, unsigned int contextId) override;

    private:
        const std::filesystem::path mScreenshotPath;
        const std::string mScreenshotFormat;
        const std::function<void(std::string)> mCallback;
    };

    class AsyncScreenCaptureOperation : public osgViewer::ScreenCaptureHandler::CaptureOperation
    {
    public:
        AsyncScreenCaptureOperation(osg::ref_ptr<SceneUtil::WorkQueue> queue,
            osg::ref_ptr<osgViewer::ScreenCaptureHandler::CaptureOperation> impl);

        ~AsyncScreenCaptureOperation();

        void stop();

        void operator()(const osg::Image& image, unsigned int contextId) override;

    private:
        const osg::ref_ptr<SceneUtil::WorkQueue> mQueue;
        const osg::ref_ptr<osgViewer::ScreenCaptureHandler::CaptureOperation> mImpl;
        Misc::ScopeGuarded<std::vector<osg::ref_ptr<SceneUtil::WorkItem>>> mWorkItems;
    };
}

#endif
