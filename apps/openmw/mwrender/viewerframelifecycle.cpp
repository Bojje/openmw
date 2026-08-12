#include "viewerframelifecycle.hpp"

#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgGA/GUIEventAdapter>
#include <osg/Stats>
#include <osg/Group>
#include <osgViewer/ViewerEventHandlers>
#include <osgViewer/Viewer>

#include <SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>
#include <components/render/textureconversion.hpp>
#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/util.hpp>
#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/stereomanager.hpp>

namespace MWRender
{
    namespace
    {
        void checkSDLError(int ret)
        {
            if (ret != 0)
                Log(Debug::Error) << "SDL error: " << SDL_GetError();
        }

        class IdentifyOpenGLOperation final : public osg::GraphicsOperation
        {
        public:
            IdentifyOpenGLOperation()
                : GraphicsOperation("IdentifyOpenGLOperation", false)
            {
            }

            void operator()(osg::GraphicsContext* /*graphicsContext*/) override
            {
                Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
                Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
                Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
                glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
            }

            int getMaxTextureImageUnits() const
            {
                if (mMaxTextureImageUnits == 0)
                    throw std::logic_error("mMaxTextureImageUnits is not initialized");
                return mMaxTextureImageUnits;
            }

        private:
            int mMaxTextureImageUnits = 0;
        };
    }

    ViewerFrameLifecycle::ViewerFrameLifecycle()
        : mViewer(new osgViewer::Viewer)
    {
        mViewer->setReleaseContextAtEndOfFrameHint(false);
        mViewer->setUseConfigureAffinity(false);
    }

    ViewerFrameLifecycle::~ViewerFrameLifecycle() = default;

    osg::Group* ViewerFrameLifecycle::sceneRoot()
    {
        if (!mSceneRoot)
        {
            mSceneRoot = new osg::Group;
            mSceneRoot->setName("World Root");
            mViewer->setSceneData(mSceneRoot);
        }
        return mSceneRoot.get();
    }

    void ViewerFrameLifecycle::initializeWindow(SDL_Window*& window, const std::filesystem::path& resourceDirectory)
    {
        const int screen = Settings::video().mScreen;
        const int width = Settings::video().mResolutionX;
        const int height = Settings::video().mResolutionY;
        const Settings::WindowMode windowMode = Settings::video().mWindowMode;
        const bool windowBorder = Settings::video().mWindowBorder;
        const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
        unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

        int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
        {
            posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
            posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        }

        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (windowMode == Settings::WindowMode::Fullscreen)
            flags |= SDL_WINDOW_FULLSCREEN;
        else if (windowMode == Settings::WindowMode::WindowedFullscreen)
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

        SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
        SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");
        if (!windowBorder)
            flags |= SDL_WINDOW_BORDERLESS;
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

        checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
        if (Debug::shouldDebugOpenGL())
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));
        if (antialiasing > 0)
        {
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
        }

        osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
        while (!graphicsWindow || !graphicsWindow->valid())
        {
            while (!window)
            {
                window = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
                if (window)
                    break;
                if (antialiasing == 0)
                    throw std::runtime_error(std::string("Failed to create SDL window: ") + SDL_GetError());
                Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                    << antialiasing / 2;
                antialiasing /= 2;
                Settings::video().mAntialiasing.set(antialiasing);
                checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            }

            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            int dw, dh;
            SDL_GL_GetDrawableSize(window, &dw, &dh);
            if (dw != w || dh != h)
                SDL_SetWindowSize(window, width / (dw / w), height / (dh / h));

            std::ifstream windowIconStream(resourceDirectory / "openmw.png", std::ios_base::in | std::ios_base::binary);
            if (!windowIconStream.fail())
            {
                osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
                if (reader)
                {
                    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
                    if (result.success())
                    {
                        const osg::ref_ptr<osg::Image> image = result.getImage();
                        const Render::TextureData iconImage = Render::makeRgba8Texture(image->s(), image->t(),
                            [image](std::uint32_t x, std::uint32_t y) {
                                const osg::Vec4f color = image->getColor(static_cast<int>(x), static_cast<int>(y));
                                return std::array<float, 4>{ color.r(), color.g(), color.b(), color.a() };
                            });
                        const auto surface = SDLUtil::imageToSurface(iconImage, true);
                        SDL_SetWindowIcon(window, surface.get());
                    }
                }
            }

            osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
            SDL_GetWindowPosition(window, &traits->x, &traits->y);
            SDL_GL_GetDrawableSize(window, &traits->width, &traits->height);
            traits->windowName = SDL_GetWindowTitle(window);
            traits->windowDecoration = !(SDL_GetWindowFlags(window) & SDL_WINDOW_BORDERLESS);
            traits->screenNum = SDL_GetWindowDisplayIndex(window);
            traits->vsync = 0;
            traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(window);

            graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
            if (!graphicsWindow->valid())
                throw std::runtime_error("Failed to create GraphicsContext");
            if (traits->samples < antialiasing)
            {
                Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples
                                    << "x instead of " << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
                graphicsWindow->closeImplementation();
                SDL_DestroyWindow(window);
                window = nullptr;
                antialiasing /= 2;
                Settings::video().mAntialiasing.set(antialiasing);
                checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                continue;
            }
            if (traits->red < 8 || traits->green < 8 || traits->blue < 8 || traits->depth < 24)
                Log(Debug::Warning) << "OpenGL framebuffer has reduced color/depth precision";
            traits->alpha = 0;
        }

        osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
        camera->setGraphicsContext(graphicsWindow);
        camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

        osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
        mViewer->setRealizeOperation(realizeOperations);
        osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
        realizeOperations->add(identifyOp);
        realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());
        if (Debug::shouldDebugOpenGL())
            realizeOperations->add(new Debug::EnableGLDebugOperation());
        realizeOperations->add(new SceneUtil::SelectDepthFormatOperation());
        realizeOperations->add(new SceneUtil::Color::SelectColorFormatOperation());

        if (Stereo::getStereo())
        {
            Stereo::Settings settings;
            settings.mMultiview = Settings::stereo().mMultiview;
            settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
            settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;
            if (Settings::stereo().mUseCustomView)
            {
                settings.mCustomView = Stereo::CustomView{
                    .mLeft = Stereo::View{
                        .pose = Stereo::Pose{ .position = osg::Vec3(Settings::stereoView().mLeftEyeOffsetX,
                            Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ),
                            .orientation = osg::Quat(Settings::stereoView().mLeftEyeOrientationX,
                                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                                Settings::stereoView().mLeftEyeOrientationW) },
                        .fov = Stereo::FieldOfView{ .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                            .angleRight = Settings::stereoView().mLeftEyeFovRight,
                            .angleUp = Settings::stereoView().mLeftEyeFovUp,
                            .angleDown = Settings::stereoView().mLeftEyeFovDown } },
                    .mRight = Stereo::View{
                        .pose = Stereo::Pose{ .position = osg::Vec3(Settings::stereoView().mRightEyeOffsetX,
                            Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ),
                            .orientation = osg::Quat(Settings::stereoView().mRightEyeOrientationX,
                                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                                Settings::stereoView().mRightEyeOrientationW) },
                        .fov = Stereo::FieldOfView{ .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                            .angleRight = Settings::stereoView().mRightEyeFovRight,
                            .angleUp = Settings::stereoView().mRightEyeFovUp,
                            .angleDown = Settings::stereoView().mRightEyeFovDown } } };
            }
            if (Settings::stereo().mUseCustomEyeResolution)
                settings.mEyeResolution
                    = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);
            realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
        }

        mViewer->realize();
        mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
            0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
        mMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();
    }

    bool ViewerFrameLifecycle::renderFrame()
    {
        mViewer->eventTraversal();
        mViewer->updateTraversal();
        mViewer->renderingTraversals();
        return true;
    }

    bool ViewerFrameLifecycle::done() const
    {
        return mViewer->done();
    }

    void ViewerFrameLifecycle::requestQuit()
    {
        mViewer->setDone(true);
    }

    double ViewerFrameLifecycle::referenceTime() const
    {
        const osg::FrameStamp* frameStamp = mViewer->getFrameStamp();
        return frameStamp ? frameStamp->getReferenceTime() : 0.0;
    }

    unsigned ViewerFrameLifecycle::frameNumber() const
    {
        const osg::FrameStamp* frameStamp = mViewer->getFrameStamp();
        return frameStamp ? frameStamp->getFrameNumber() : 0;
    }

    void ViewerFrameLifecycle::advanceFrame(double simulationTime)
    {
        mViewer->advance(simulationTime);
    }
}
