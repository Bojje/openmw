#include "nifmeshmanager.hpp"

#include <stdexcept>
#include <algorithm>
#include <cmath>

#include <components/nif/controller.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/meshconverter.hpp>

#include "niffilemanager.hpp"

namespace Resource
{
    NifMeshManager::NifMeshManager(NifFileManager* nifFileManager)
        : mNifFileManager(nifFileManager)
    {
    }

    NifMeshManager::~NifMeshManager() = default;

    std::shared_ptr<const NifMeshManager::Meshes> NifMeshManager::get(VFS::Path::NormalizedView name)
    {
        return get(mNifFileManager->get(name));
    }

    std::shared_ptr<const NifMeshManager::Meshes> NifMeshManager::get(const Nif::NIFFilePtr& file)
    {
        if (!file)
            throw std::invalid_argument("NifMeshManager cannot convert a null NIF file");

        const std::string key = file->mPath.value();
        {
            std::lock_guard lock(mMutex);
            ++mStats.mGet;
            const auto found = mCache.find(key);
            if (found != mCache.end())
            {
                ++mStats.mHit;
                return found->second.mMeshes;
            }
        }

        auto meshes = std::make_shared<Meshes>(Nif::collectMeshInstances(Nif::FileView(*file)));

        std::lock_guard lock(mMutex);
        const auto [it, inserted] = mCache.emplace(key, CacheItem{ meshes });
        return inserted ? std::move(meshes) : it->second.mMeshes;
    }

    std::optional<float> NifMeshManager::getAnimationDuration(VFS::Path::NormalizedView name)
    {
        return getAnimationDuration(mNifFileManager->get(name));
    }

    std::optional<float> NifMeshManager::getAnimationDuration(const Nif::NIFFilePtr& file) const
    {
        if (!file)
            return std::nullopt;
        std::optional<float> result;
        for (const std::unique_ptr<Nif::Record>& record : file->mRecords)
        {
            const auto* controller = dynamic_cast<const Nif::NiTimeController*>(record.get());
            if (controller == nullptr || !std::isfinite(controller->mTimeStop))
                continue;
            const float duration = std::max(0.f, controller->mTimeStop - controller->mTimeStart);
            result = std::max(result.value_or(0.f), duration);
        }
        return result;
    }

    std::optional<float> NifMeshManager::getAnimationDuration(
        VFS::Path::NormalizedView name, std::string_view group, std::string_view startKey, std::string_view stopKey)
    {
        return getAnimationDuration(mNifFileManager->get(name), group, startKey, stopKey);
    }

    std::optional<float> NifMeshManager::getAnimationDuration(const Nif::NIFFilePtr& file, std::string_view group,
        std::string_view startKey, std::string_view stopKey) const
    {
        const std::optional<float> duration = getAnimationDuration(file);
        if (!duration || group.empty() || startKey.empty() || stopKey.empty())
            return duration;

        const std::string start = std::string(group) + ": " + std::string(startKey);
        const std::string stop = std::string(group) + ": " + std::string(stopKey);
        const std::optional<float> startTime = Nif::findTextKeyTime(Nif::FileView(*file), start);
        const std::optional<float> stopTime = Nif::findTextKeyTime(Nif::FileView(*file), stop);
        if (!startTime || !stopTime || *stopTime <= *startTime)
            return duration;
        return *stopTime - *startTime;
    }

    std::vector<Render::Mat4> NifMeshManager::getBonePose(
        VFS::Path::NormalizedView name, float time, std::span<const std::string> boneNames, std::string_view group)
    {
        return getBonePose(mNifFileManager->get(name), time, boneNames, group);
    }

    std::vector<Render::Mat4> NifMeshManager::getBonePose(
        const Nif::NIFFilePtr& file, float time, std::span<const std::string> boneNames, std::string_view group) const
    {
        if (!file)
            return {};
        return Nif::collectBonePose(Nif::FileView(*file), boneNames, time, group);
    }

    void NifMeshManager::updateCache(double referenceTime)
    {
        std::lock_guard lock(mMutex);
        const double expiryTime = referenceTime - mExpiryDelay;
        std::erase_if(mCache, [&](auto& item) {
            CacheItem& cacheItem = item.second;
            if (cacheItem.mMeshes.use_count() > 1 || cacheItem.mLastUsage == 0.0)
                cacheItem.mLastUsage = referenceTime;
            if (cacheItem.mLastUsage > expiryTime)
                return false;
            ++mStats.mExpired;
            return true;
        });
    }

    void NifMeshManager::clearCache()
    {
        std::lock_guard lock(mMutex);
        mCache.clear();
    }

    void NifMeshManager::setExpiryDelay(double expiryDelay)
    {
        std::lock_guard lock(mMutex);
        mExpiryDelay = expiryDelay;
    }

    CacheStats NifMeshManager::getStats() const
    {
        std::lock_guard lock(mMutex);
        CacheStats result = mStats;
        result.mSize = mCache.size();
        return result;
    }

}
