#include "nifmeshmanager.hpp"

#include <array>
#include <stdexcept>
#include <algorithm>
#include <cmath>

#include <components/nif/controller.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/meshconverter.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/recursivedirectoryiterator.hpp>

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

    std::vector<Nif::NIFFilePtr> NifMeshManager::getAnimationSources(VFS::Path::NormalizedView name) const
    {
        const std::array<VFS::Path::Normalized, 1> names{ VFS::Path::Normalized(name) };
        return getAnimationSources(names);
    }

    std::vector<Nif::NIFFilePtr> NifMeshManager::getAnimationSources(
        std::span<const VFS::Path::Normalized> names) const
    {
        std::vector<Nif::NIFFilePtr> result;
        std::vector<VFS::Path::Normalized> paths;
        const auto addPath = [&](VFS::Path::Normalized path) {
            if (!mNifFileManager->getVFS()->exists(path)
                || std::any_of(paths.begin(), paths.end(), [&](const VFS::Path::Normalized& existing) {
                       return existing == path;
                   }))
                return;
            paths.push_back(std::move(path));
        };

        for (const VFS::Path::Normalized& name : names)
        {
            addPath(name);
            VFS::Path::Normalized sibling(name);
            if (sibling.changeExtension(VFS::Path::ExtensionView("kf")))
                addPath(std::move(sibling));

            constexpr std::string_view meshes = "meshes/";
            if (name.value().starts_with(meshes))
            {
                std::string directory = "animations/";
                directory += name.value().substr(meshes.size());
                const std::size_t extension = directory.find_last_of(VFS::Path::extensionSeparator);
                if (extension != std::string::npos)
                {
                    directory.resize(extension + 1);
                    directory.back() = VFS::Path::separator;
                    std::vector<std::string> additional;
                    for (const VFS::Path::Normalized& path
                        : mNifFileManager->getVFS()->getRecursiveDirectoryIterator(directory))
                        if (path.extension() == VFS::Path::ExtensionView("kf"))
                            additional.emplace_back(path.value());
                    std::sort(additional.begin(), additional.end());
                    for (const std::string& path : additional)
                        addPath(VFS::Path::Normalized(path));
                }
            }
        }

        for (const VFS::Path::Normalized& path : paths)
        {
            const Nif::NIFFilePtr file = mNifFileManager->get(path);
            if (file)
                result.push_back(file);
        }
        return result;
    }

    Nif::NIFFilePtr NifMeshManager::getAnimationSource(
        VFS::Path::NormalizedView name, std::string_view group) const
    {
        const std::vector<Nif::NIFFilePtr> files = getAnimationSources(name);
        return getAnimationSource(files, group);
    }

    Nif::NIFFilePtr NifMeshManager::getAnimationSource(
        std::span<const Nif::NIFFilePtr> files, std::string_view group) const
    {
        Nif::NIFFilePtr result;
        for (const Nif::NIFFilePtr& file : files)
            if (hasAnimationGroup(file, group))
                result = file;
        return result;
    }

    std::optional<float> NifMeshManager::getAnimationDuration(VFS::Path::NormalizedView name)
    {
        const std::vector<Nif::NIFFilePtr> files = getAnimationSources(name);
        return getAnimationDuration(files);
    }

    std::optional<float> NifMeshManager::getAnimationDuration(std::span<const Nif::NIFFilePtr> files) const
    {
        std::optional<float> result;
        for (const Nif::NIFFilePtr& file : files)
            if (const std::optional<float> duration = getAnimationDuration(file))
                result = std::max(result.value_or(0.f), *duration);
        return result;
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

    bool NifMeshManager::hasAnimationGroup(const Nif::NIFFilePtr& file, std::string_view group) const
    {
        if (!file)
            return false;
        if (group.empty())
            return getAnimationDuration(file).has_value();
        return !getAnimationTextKeys(file, group).empty();
    }

    std::optional<float> NifMeshManager::getAnimationDuration(
        VFS::Path::NormalizedView name, std::string_view group, std::string_view startKey, std::string_view stopKey)
    {
        const std::vector<Nif::NIFFilePtr> files = getAnimationSources(name);
        return getAnimationDuration(files, group, startKey, stopKey);
    }

    std::optional<float> NifMeshManager::getAnimationDuration(const Nif::NIFFilePtr& file, std::string_view group,
        std::string_view startKey, std::string_view stopKey) const
    {
        const std::optional<float> duration = getAnimationDuration(file);
        if (!duration || group.empty())
            return duration;
        if (!hasAnimationGroup(file, group))
            return std::nullopt;
        if (startKey.empty() || stopKey.empty())
            return duration;

        const std::string start = std::string(group) + ": " + std::string(startKey);
        const std::string stop = std::string(group) + ": " + std::string(stopKey);
        const std::optional<float> startTime = Nif::findTextKeyTime(Nif::FileView(*file), start);
        const std::optional<float> stopTime = Nif::findTextKeyTime(Nif::FileView(*file), stop);
        if (!startTime || !stopTime || *stopTime <= *startTime)
            return duration;
        return *stopTime - *startTime;
    }

    std::optional<float> NifMeshManager::getAnimationDuration(
        std::span<const Nif::NIFFilePtr> files, std::string_view group, std::string_view startKey,
        std::string_view stopKey) const
    {
        return getAnimationDuration(getAnimationSource(files, group), group, startKey, stopKey);
    }

    std::vector<Render::AnimationTextKey> NifMeshManager::getAnimationTextKeys(
        VFS::Path::NormalizedView name, std::string_view group, std::string_view startKey,
        std::string_view stopKey)
    {
        const std::vector<Nif::NIFFilePtr> files = getAnimationSources(name);
        return getAnimationTextKeys(files, group, startKey, stopKey);
    }

    std::vector<Render::AnimationTextKey> NifMeshManager::getAnimationTextKeys(
        const Nif::NIFFilePtr& file, std::string_view group, std::string_view startKey,
        std::string_view stopKey) const
    {
        if (!file || group.empty())
            return {};

        float segmentStart = 0.f;
        if (!startKey.empty())
            segmentStart = Nif::findTextKeyTime(Nif::FileView(*file),
                std::string(group) + ": " + std::string(startKey)).value_or(0.f);

        std::optional<float> segmentStop;
        if (!stopKey.empty())
            segmentStop = Nif::findTextKeyTime(Nif::FileView(*file),
                std::string(group) + ": " + std::string(stopKey));

        std::vector<Render::AnimationTextKey> result;
        for (Render::AnimationTextKey key : Nif::collectTextKeys(Nif::FileView(*file), group))
        {
            if (key.time < segmentStart || (segmentStop && key.time > *segmentStop))
                continue;
            key.time -= segmentStart;
            result.push_back(std::move(key));
        }
        return result;
    }

    std::vector<Render::AnimationTextKey> NifMeshManager::getAnimationTextKeys(
        std::span<const Nif::NIFFilePtr> files, std::string_view group, std::string_view startKey,
        std::string_view stopKey) const
    {
        return getAnimationTextKeys(getAnimationSource(files, group), group, startKey, stopKey);
    }

    std::vector<Render::Mat4> NifMeshManager::getBonePose(
        VFS::Path::NormalizedView name, float time, std::span<const std::string> boneNames, std::string_view group,
        std::string_view startKey, std::string_view stopKey)
    {
        const std::vector<Nif::NIFFilePtr> files = getAnimationSources(name);
        return getBonePose(files, time, boneNames, group, startKey, stopKey);
    }

    std::vector<Render::Mat4> NifMeshManager::getBonePose(
        std::span<const Nif::NIFFilePtr> files, float time, std::span<const std::string> boneNames,
        std::string_view group, std::string_view startKey, std::string_view stopKey) const
    {
        std::vector<Nif::FileView> views;
        views.reserve(files.size());
        for (const Nif::NIFFilePtr& file : files)
            views.emplace_back(*file);
        return Nif::collectBonePose(views, boneNames, time, group, startKey, stopKey);
    }

    std::vector<Render::Mat4> NifMeshManager::getBonePose(
        const Nif::NIFFilePtr& file, float time, std::span<const std::string> boneNames, std::string_view group,
        std::string_view startKey, std::string_view stopKey) const
    {
        if (!file)
            return {};
        return Nif::collectBonePose(Nif::FileView(*file), boneNames, time, group, startKey, stopKey);
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
