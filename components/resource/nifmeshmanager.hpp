#ifndef OPENMW_COMPONENTS_RESOURCE_NIFMESHMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_NIFMESHMANAGER_H

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <components/nif/niffile.hpp>
#include <components/render/mesh.hpp>

#include "cachestats.hpp"

namespace Resource
{
    class NifFileManager;

    /// @brief Caches renderer-neutral mesh instances converted from NIF files.
    /// @note May be used from any thread. Cached meshes do not retain the parsed NIF file.
    class NifMeshManager
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

        void updateCache(double referenceTime);
        void clearCache();
        void setExpiryDelay(double expiryDelay);
        CacheStats getStats() const;
    };
}

#endif
