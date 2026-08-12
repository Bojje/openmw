#ifndef OPENMW_COMPONENTS_RESOURCE_NEUTRALTEXTUREMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_NEUTRALTEXTUREMANAGER_H

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <components/render/texture.hpp>
#include <components/vfs/pathutil.hpp>

namespace VFS
{
    class Manager;
}

namespace Resource
{
    /// Loads common Morrowind texture formats into backend-neutral RGBA8 data.
    /// The cache owns decoded bytes and never exposes OSG image objects.
    class NeutralTextureManager
    {
    public:
        explicit NeutralTextureManager(const VFS::Manager* vfs);

        std::shared_ptr<const Render::TextureData> get(VFS::Path::NormalizedView path);
        void clearCache();

    private:
        const VFS::Manager* mVfs;
        std::mutex mMutex;
        std::unordered_map<std::string, std::shared_ptr<const Render::TextureData>> mCache;
    };
}

#endif
