#ifndef OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H
#define OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H

#include <filesystem>
#include <osg/ref_ptr>

#include <components/render/frame.hpp>

namespace osgViewer
{
    class Viewer;
}

namespace osg
{
    class Group;
}

struct SDL_Window;

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
        int initializeWindow(SDL_Window*& window, const std::filesystem::path& resourceDirectory);
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
    };
}

#endif
