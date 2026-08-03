#ifndef OPENMW_MWGUI_VIDEOWIDGET_H
#define OPENMW_MWGUI_VIDEOWIDGET_H

#include <atomic>
#include <memory>

#include <MyGUI_Widget.h>

#include <osg/ref_ptr>

namespace osg
{
    class Image;
    class Texture2D;
}

namespace Video
{
    class VideoPlayer;
}

namespace VFS
{
    class Manager;
}

namespace MWGui
{

    /**
     * Widget that plays a video.
     */
    class VideoWidget : public MyGUI::Widget
    {
    public:
        MYGUI_RTTI_DERIVED(VideoWidget)

        VideoWidget();

        ~VideoWidget();

        /// Set the VFS (virtual file system) to find the videos on.
        void setVFS(const VFS::Manager* vfs);

        void playVideo(const std::string& video);

        int getVideoWidth();
        int getVideoHeight();

        /// @return Is the video still playing?
        bool update();

        void commitFrame();

        /// Return true if a video is currently playing and it has an audio stream.
        bool hasAudioStream();

        /// Stop video and free resources (done automatically on destruction)
        void stop();

        void pause();
        void resume();
        bool isPaused() const;

        /// Adjust the coordinates of this video widget relative to its parent,
        /// based on the dimensions of the playing video.
        /// @param stretch Stretch the video to fill the whole screen? If false,
        ///                black bars may be added to fix the aspect ratio.
        void autoResize(bool stretch);

    private:
        /// Point the widget at the frame the player has most recently committed. Does nothing under
        /// the OSG platform beyond the first call, where the widget is pointed at the video texture
        /// and follows it by itself.
        void refreshTexture();

        const VFS::Manager* mVFS;
        std::unique_ptr<MyGUI::ITexture> mTexture;
        osg::ref_ptr<osg::Texture2D> mVideoTexture;
        // The player alternates between two images, so the frame it is showing is identified by the
        // pair and not by either half: the pointer repeats every other frame and the modified count
        // belongs to whichever image it is.
        osg::Image* mUploadedImage = nullptr;
        unsigned int mUploadedModifiedCount = 0;
        // Set by playVideo, which is not always on the main thread, and acted on by refreshTexture,
        // which is. A new video means a new frame texture even if the pair below happens to repeat.
        std::atomic<bool> mRebind{ false };
        std::unique_ptr<Video::VideoPlayer> mPlayer;
    };

}

#endif
