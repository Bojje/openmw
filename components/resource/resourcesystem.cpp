#include "resourcesystem.hpp"

#include <algorithm>

#include "animblendrulesmanager.hpp"
#include "bgsmfilemanager.hpp"
#include "cachemanager.hpp"
#include "cachestats.hpp"
#include "imagemanager.hpp"
#include "keyframemanager.hpp"
#include "niffilemanager.hpp"
#include "nifmeshmanager.hpp"
#include "neutraltexturemanager.hpp"
#include "scenemanager.hpp"

namespace Resource
{

    ResourceSystem::ResourceSystem(
        const VFS::Manager* vfs, double expiryDelay, const ToUTF8::StatelessUtf8Encoder* encoder, Backend backend)
        : mVFS(vfs)
        , mBackend(backend)
    {
        mNifFileManager = std::make_unique<NifFileManager>(vfs, encoder);
        mNifMeshManager = std::make_unique<NifMeshManager>(mNifFileManager.get());

        if (backend == Backend::Neutral)
            mNeutralTextureManager = std::make_unique<NeutralTextureManager>(vfs);

        if (backend == Backend::Osg)
        {
            mBgsmFileManager = std::make_unique<BgsmFileManager>(vfs, expiryDelay);
            mImageManager = std::make_unique<ImageManager>(vfs, expiryDelay);
            mAnimBlendRulesManager = std::make_unique<AnimBlendRulesManager>(vfs, expiryDelay);
            mSceneManager = std::make_unique<SceneManager>(
                vfs, mImageManager.get(), mNifFileManager.get(), mBgsmFileManager.get(), expiryDelay);
            mKeyframeManager = std::make_unique<KeyframeManager>(vfs, mSceneManager.get(), expiryDelay, encoder);
        }

        mCacheManagers.push_back(mNifFileManager.get());
        mCacheManagers.push_back(mNifMeshManager.get());
        if (mBgsmFileManager)
            addResourceManager(mBgsmFileManager.get());
        if (mKeyframeManager)
            addResourceManager(mKeyframeManager.get());
        // note, scene references images so add images afterwards for correct implementation of updateCache()
        if (mSceneManager)
            addResourceManager(mSceneManager.get());
        if (mImageManager)
            addResourceManager(mImageManager.get());
        if (mAnimBlendRulesManager)
            addResourceManager(mAnimBlendRulesManager.get());
    }

    ResourceSystem::~ResourceSystem()
    {
        // this has to be defined in the .cpp file as we can't delete incomplete types

        mResourceManagers.clear();
    }

    ResourceSystem::Backend ResourceSystem::backend() const
    {
        return mBackend;
    }

    SceneManager* ResourceSystem::getSceneManager()
    {
        return mSceneManager.get();
    }

    ImageManager* ResourceSystem::getImageManager()
    {
        return mImageManager.get();
    }

    NeutralTextureManager* ResourceSystem::getNeutralTextureManager()
    {
        return mNeutralTextureManager.get();
    }

    BgsmFileManager* ResourceSystem::getBgsmFileManager()
    {
        return mBgsmFileManager.get();
    }

    NifFileManager* ResourceSystem::getNifFileManager()
    {
        return mNifFileManager.get();
    }

    NifMeshManager* ResourceSystem::getNifMeshManager()
    {
        return mNifMeshManager.get();
    }

    KeyframeManager* ResourceSystem::getKeyframeManager()
    {
        return mKeyframeManager.get();
    }

    AnimBlendRulesManager* ResourceSystem::getAnimBlendRulesManager()
    {
        return mAnimBlendRulesManager.get();
    }

    void ResourceSystem::setExpiryDelay(double expiryDelay)
    {
        for (CacheManager* const cacheManager : mCacheManagers)
            cacheManager->setExpiryDelay(expiryDelay);
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->setExpiryDelay(expiryDelay);

        // NIF files aren't needed any more once the converted objects are cached in SceneManager / BulletShapeManager,
        // so no point in using an expiry delay
        mNifFileManager->setExpiryDelay(0.0);
    }

    void ResourceSystem::updateCache(double referenceTime)
    {
        for (CacheManager* const cacheManager : mCacheManagers)
            cacheManager->updateCache(referenceTime);
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->updateCache(referenceTime);
    }

    void ResourceSystem::clearCache()
    {
        if (mNeutralTextureManager)
            mNeutralTextureManager->clearCache();
        for (CacheManager* const cacheManager : mCacheManagers)
            cacheManager->clearCache();
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->clearCache();
    }

    void ResourceSystem::addResourceManager(BaseResourceManager* resourceMgr)
    {
        mResourceManagers.push_back(resourceMgr);
    }

    void ResourceSystem::removeResourceManager(BaseResourceManager* resourceMgr)
    {
        std::vector<BaseResourceManager*>::iterator found
            = std::find(mResourceManagers.begin(), mResourceManagers.end(), resourceMgr);
        if (found != mResourceManagers.end())
            mResourceManagers.erase(found);
    }

    const VFS::Manager* ResourceSystem::getVFS() const
    {
        return mVFS;
    }

    void ResourceSystem::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Nif", frameNumber, mNifFileManager->getStats(), *stats);
        Resource::reportStats("NifMesh", frameNumber, mNifMeshManager->getStats(), *stats);
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->reportStats(frameNumber, stats);
    }

    void ResourceSystem::releaseGLObjects(osg::State* state)
    {
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->releaseGLObjects(state);
    }

}
