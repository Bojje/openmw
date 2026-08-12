#ifndef OPENMW_COMPONENTS_RESOURCE_NIFMESHMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_NIFMESHMANAGER_H

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <components/nif/niffile.hpp>
#include <components/render/mesh.hpp>

#include "cachemanager.hpp"
#include "cachestats.hpp"

namespace Resource
{
    class NifFileManager;

    /// @brief Caches renderer-neutral mesh instances converted from NIF files.
    /// @note May be used from any thread. Cached meshes do not retain the parsed NIF file.
    class NifMeshManager : public CacheManager
    {
    public:
        using Meshes = std::vector<Render::MeshInstance>;

    private:
        struct CacheItem
        {
            std::shared_ptr<const Meshes> mMeshes;
            double mLastUsage = 0.0;
        };

        NifFileManager* mNifFileManager;
        mutable std::mutex mMutex;
        std::map<std::string, CacheItem, std::less<>> mCache;
        double mExpiryDelay = 0.0;
        CacheStats mStats;

    public:
        explicit NifMeshManager(NifFileManager* nifFileManager);
        ~NifMeshManager();

        /// Retrieve converted meshes from the cache, or load and convert the NIF if needed.
        std::shared_ptr<const Meshes> get(VFS::Path::NormalizedView name);

        /// Convert an already-loaded NIF, using the same path-keyed cache.
        std::shared_ptr<const Meshes> get(const Nif::NIFFilePtr& file);

        /// Return the longest controller interval in a NIF, when one exists.
        /// This is renderer-neutral metadata used by world-owned effects.
        std::optional<float> getAnimationDuration(VFS::Path::NormalizedView name);
        std::optional<float> getAnimationDuration(const Nif::NIFFilePtr& file) const;

        void updateCache(double referenceTime) override;
        void clearCache() override;
        void setExpiryDelay(double expiryDelay) override;
        CacheStats getStats() const;
    };
}

#endif
