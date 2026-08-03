#include "videowidget.hpp"

#include <osg-ffmpeg-videoplayer/videoplayer.hpp>

#include <MyGUI_ITexture.h>
#include <MyGUI_RenderManager.h>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/vfs/manager.hpp>

#include "../mwsound/movieaudiofactory.hpp"

#include "guitexture.hpp"

namespace MWGui
{

    VideoWidget::VideoWidget()
        : mVFS(nullptr)
    {
        mPlayer = std::make_unique<Video::VideoPlayer>();
        setNeedKeyFocus(true);
    }

    VideoWidget::~VideoWidget() = default;

    void VideoWidget::setVFS(const VFS::Manager* vfs)
    {
        mVFS = vfs;
    }

    void VideoWidget::playVideo(const std::string& video)
    {
        mPlayer->setAudioFactory(new MWSound::MovieAudioFactory());

        Files::IStreamPtr videoStream;
        try
        {
            videoStream = mVFS->get(video);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Failed to open video: " << e.what();
            return;
        }

        mPlayer->playVideo(std::move(videoStream), video);

        // No texture work here, and specifically not from this call. The main menu's background
        // video restarts itself from its own thread, so playVideo is not always on the main thread,
        // and creating or freeing an interface texture off it would race the frame being drawn.
        // commitFrame is called from the main thread in every one of these loops and does it there.
        mRebind = true;
    }

    void VideoWidget::refreshTexture()
    {
        mVideoTexture = mPlayer->getVideoTexture();
        if (!mVideoTexture)
            return;

        const bool rebind = mRebind.exchange(false);
        osg::Image* image = mVideoTexture->getImage();
        if (usingVulkanGuiPlatform())
        {
            // Every frame of the video is a new upload, because there is no shared texture between
            // the two backends -- the same trade as everywhere else the interface reads from OSG,
            // and here it costs one texture per video frame rather than per interface change.
            if (image == nullptr || image->data() == nullptr)
                return;
            if (!rebind && mTexture && image == mUploadedImage
                && image->getModifiedCount() == mUploadedModifiedCount)
                return;
            mUploadedImage = image;
            mUploadedModifiedCount = image->getModifiedCount();
        }
        else if (mTexture && !rebind)
        {
            return;
        }

        // Order matters: the widget must stop pointing at the old texture before it is freed.
        setRenderItemTexture(nullptr);
        mTexture = createGuiTexture(mVideoTexture, image, "video");
        if (!mTexture)
            return;

        setRenderItemTexture(mTexture.get());
        // Both the widget and the video frame are Y-down, so this UV is not inverted
        getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
    }

    int VideoWidget::getVideoWidth()
    {
        return mPlayer->getVideoWidth();
    }

    int VideoWidget::getVideoHeight()
    {
        return mPlayer->getVideoHeight();
    }

    bool VideoWidget::update()
    {
        return mPlayer->update();
    }

    void VideoWidget::commitFrame()
    {
        mPlayer->commitFrame();
        refreshTexture();
    }

    void VideoWidget::stop()
    {
        // Before close(), which drops the player's reference to the frame this widget is drawing.
        setRenderItemTexture(nullptr);
        mTexture.reset();
        mVideoTexture = nullptr;
        mUploadedImage = nullptr;

        mPlayer->close();
    }

    void VideoWidget::pause()
    {
        mPlayer->pause();
    }

    void VideoWidget::resume()
    {
        mPlayer->play();
    }

    bool VideoWidget::isPaused() const
    {
        return mPlayer->isPaused();
    }

    bool VideoWidget::hasAudioStream()
    {
        return mPlayer->hasAudioStream();
    }

    void VideoWidget::autoResize(bool stretch)
    {
        MyGUI::IntSize screenSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (getParent())
            screenSize = getParent()->getSize();

        if (getVideoHeight() > 0 && !stretch)
        {
            double imageaspect = static_cast<double>(getVideoWidth()) / getVideoHeight();

            int leftPadding = std::max(0, static_cast<int>(screenSize.width - screenSize.height * imageaspect) / 2);
            int topPadding = std::max(0, static_cast<int>(screenSize.height - screenSize.width / imageaspect) / 2);

            setCoord(leftPadding, topPadding, screenSize.width - leftPadding * 2, screenSize.height - topPadding * 2);
        }
        else
            setCoord(0, 0, screenSize.width, screenSize.height);
    }

}
