#ifndef OPENMW_COMPONENTS_RESOURCE_CACHEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_CACHEMANAGER_H

namespace Resource
{
    /// Renderer-neutral cache lifecycle used by resources shared by all backends.
    class CacheManager
    {
    public:
        virtual ~CacheManager() = default;

        virtual void updateCache(double referenceTime) = 0;
        virtual void clearCache() = 0;
        virtual void setExpiryDelay(double expiryDelay) = 0;
    };
}

#endif
