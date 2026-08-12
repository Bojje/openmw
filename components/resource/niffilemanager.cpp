#include "niffilemanager.hpp"

#include <components/vfs/manager.hpp>

namespace Resource
{

    NifFileManager::NifFileManager(const VFS::Manager* vfs, const ToUTF8::StatelessUtf8Encoder* encoder)
        : mVFS(vfs)
        , mEncoder(encoder)
    {
    }

    NifFileManager::~NifFileManager() = default;

    Nif::NIFFilePtr NifFileManager::get(VFS::Path::NormalizedView name)
    {
        {
            std::lock_guard lock(mMutex);
            ++mStats.mGet;
            const auto found = mCache.find(name.value());
            if (found != mCache.end())
            {
                ++mStats.mHit;
                return found->second.mFile;
            }
        }

        auto file = std::make_shared<Nif::NIFFile>(name);
        Nif::Reader reader(*file, mEncoder);
        reader.parse(mVFS->get(name));

        std::lock_guard lock(mMutex);
        const auto [it, inserted] = mCache.emplace(name.value(), CacheItem{ file });
        return inserted ? file : it->second.mFile;
    }

    void NifFileManager::updateCache(double referenceTime)
    {
        std::lock_guard lock(mMutex);
        const double expiryTime = referenceTime - mExpiryDelay;
        std::erase_if(mCache, [&](auto& item) {
            CacheItem& cacheItem = item.second;
            if (cacheItem.mFile.use_count() > 1 || cacheItem.mLastUsage == 0.0)
                cacheItem.mLastUsage = referenceTime;
            if (cacheItem.mLastUsage > expiryTime)
                return false;
            ++mStats.mExpired;
            return true;
        });
    }

    void NifFileManager::clearCache()
    {
        std::lock_guard lock(mMutex);
        mCache.clear();
    }

    void NifFileManager::setExpiryDelay(double expiryDelay)
    {
        std::lock_guard lock(mMutex);
        mExpiryDelay = expiryDelay;
    }

    CacheStats NifFileManager::getStats() const
    {
        std::lock_guard lock(mMutex);
        CacheStats statsCopy = mStats;
        statsCopy.mSize = mCache.size();
        return statsCopy;
    }

}
