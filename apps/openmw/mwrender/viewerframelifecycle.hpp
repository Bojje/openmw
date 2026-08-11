#ifndef OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H
#define OPENMW_MWRENDER_VIEWER_FRAME_LIFECYCLE_H

#include <functional>

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
        explicit ViewerFrameLifecycle(osgViewer::Viewer& viewer,
            std::function<void(Render::SceneData&)> synchronizeScene = {});

        bool renderFrame() override;
        bool renderFrame(const Render::SceneSubmission& submission) override;
        bool consumesSceneSubmission() const override { return false; }
        void synchronizeScene(Render::SceneData& sceneData) override;
        void advanceFrame(double simulationTime) override;

    private:
        osgViewer::Viewer& mViewer;
        std::function<void(Render::SceneData&)> mSynchronizeScene;
    };
}

#endif
