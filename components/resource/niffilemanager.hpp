#ifndef OPENMW_COMPONENTS_RESOURCE_NIFFILEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_NIFFILEMANAGER_H

#include <map>
#include <mutex>

#include <components/nif/niffile.hpp>

#include "cachemanager.hpp"
#include "cachestats.hpp"

namespace ToUTF8
{
    class StatelessUtf8Encoder;
}

namespace VFS
{
    class Manager;
}

namespace Resource
{

    /// @brief Handles caching of NIFFiles.
    /// @note May be used from any thread.
    class NifFileManager : public CacheManager
    {
        struct CacheItem
        {
            Nif::NIFFilePtr mFile;
            double mLastUsage = 0.0;
        };

        const VFS::Manager* mVFS;
        const ToUTF8::StatelessUtf8Encoder* mEncoder;
        mutable std::mutex mMutex;
        std::map<std::string, CacheItem, std::less<>> mCache;
        double mExpiryDelay = 0.0;
        CacheStats mStats;

    public:
        NifFileManager(const VFS::Manager* vfs, const ToUTF8::StatelessUtf8Encoder* encoder);
        ~NifFileManager() override;

        /// Retrieve a NIF file from the cache, or load it from the VFS if not cached yet.
        /// @note For performance reasons the NifFileManager does not handle case folding, needs
        /// to be done in advance by other managers accessing the NifFileManager.
        Nif::NIFFilePtr get(VFS::Path::NormalizedView name);

        const VFS::Manager* getVFS() const { return mVFS; }

        void updateCache(double referenceTime) override;
        void clearCache() override;
        void setExpiryDelay(double expiryDelay) override;
        CacheStats getStats() const;
    };

}

#endif
