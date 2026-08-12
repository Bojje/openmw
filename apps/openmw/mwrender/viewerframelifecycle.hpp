#ifndef OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H
#define OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <osg/ref_ptr>

#include <components/render/frame.hpp>

namespace osgViewer
{
    class Viewer;
    class ScreenCaptureHandler;
}

namespace Resource
{
    class Profiler;
}

namespace osg
{
    class Group;
    class Stats;
}

struct SDL_Window;

namespace VFS
{
    class Manager;
}

namespace SceneUtil
{
    class AsyncScreenCaptureOperation;
    class WorkQueue;
}

namespace MWRender
{
    /// OSG bootstrap owner used before the game World and RenderingManager exist.
    /// It keeps viewer traversal out of engine and GUI orchestration code.
    class ViewerFrameLifecycle final : public Render::FrameLifecycle
    {
    public:
        ViewerFrameLifecycle();
        ~ViewerFrameLifecycle() override;

        ViewerFrameLifecycle(const ViewerFrameLifecycle&) = delete;
        ViewerFrameLifecycle& operator=(const ViewerFrameLifecycle&) = delete;

        osgViewer::Viewer* viewer() const { return mViewer.get(); }

        // Create and realize the OSG window owned by this lifecycle. The
        // engine receives only the resulting SDL handle and GL capability.
        void initializeWindow(SDL_Window*& window, const std::filesystem::path& resourceDirectory);
        void initializeScreenCapture(osg::ref_ptr<SceneUtil::WorkQueue> workQueue,
            const std::filesystem::path& screenshotPath, const std::string& screenshotFormat,
            std::function<void(std::string)> callback);
        void captureNextFrame();
        void initializeStatsHandlers(const VFS::Manager& vfs, bool writeToFile,
            const std::function<void(Resource::Profiler&)>& configureProfiler);
        void reportStats(unsigned frameNumber, std::ostream& stream) const;
        osg::Stats* stats() const;
        int maxTextureImageUnits() const { return mMaxTextureImageUnits; }
        osg::Group* sceneRoot();

        Render::FrameLifecycle::Backend backend() const override { return Render::FrameLifecycle::Backend::Osg; }
        bool renderFrame() override;
        bool consumesSceneSubmission() const override { return false; }
        bool done() const override;
        void requestQuit() override;
        double referenceTime() const override;
        unsigned frameNumber() const override;
        void advanceFrame(double simulationTime) override;

    private:
        osg::ref_ptr<osgViewer::Viewer> mViewer;
        osg::ref_ptr<osg::Group> mSceneRoot;
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> mScreenCaptureHandler;
        osg::ref_ptr<SceneUtil::AsyncScreenCaptureOperation> mScreenCaptureOperation;
        int mMaxTextureImageUnits = 0;
    };
}

#endif
