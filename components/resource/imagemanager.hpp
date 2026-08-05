#ifndef OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H

#include <memory>

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>
#include <components/render/texture.hpp>

#include "resourcemanager.hpp"

namespace osgDB
{
    class Options;
}

namespace Resource
{

    /// @brief Handles loading/caching of Images.
    /// @note May be used from any thread.
    class ImageManager : public ResourceManager
    {
    public:
        explicit ImageManager(const VFS::Manager* vfs, double expiryDelay);
        ~ImageManager();

        /// Create or retrieve an Image
        /// Returns the dummy image if the given image is not found.
        osg::ref_ptr<osg::Image> getImage(VFS::Path::NormalizedView path, bool disableFlip = false);

        /// Return a renderer-neutral RGBA8 copy of an image.
        std::shared_ptr<const Render::TextureData> getRenderTexture(VFS::Path::NormalizedView path);

        osg::Image* getWarningImage();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

    private:
        osg::ref_ptr<osg::Image> mWarningImage;
        osg::ref_ptr<osgDB::Options> mOptions;

        ImageManager(const ImageManager&);
        void operator=(const ImageManager&);
    };

}

#endif
