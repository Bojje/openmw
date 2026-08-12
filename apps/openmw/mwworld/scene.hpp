#ifndef GAME_MWWORLD_SCENE_H
#define GAME_MWWORLD_SCENE_H

#include <osg/ref_ptr>

#include "positioncellgrid.hpp"
#include "ptr.hpp"

#include <array>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm/exteriorcelllocation.hpp>
#include <components/misc/constants.hpp>
#include <components/render/frame.hpp>
#include <components/render/submission.hpp>
#include <components/render/world.hpp>

namespace osg
{
    class Vec3f;
    class Stats;
}

namespace osgUtil
{
    class IncrementalCompileOperation;
}

namespace ESM
{
    struct Position;
}

namespace Files
{
    class Collections;
}

namespace Loading
{
    class Listener;
}

namespace DetourNavigator
{
    struct Navigator;
    class UpdateGuard;
}

namespace MWRender
{
    class LandManager;
    class ObjectPaging;
    class SkyManager;
    class RenderingManager;
}

namespace Terrain
{
    class RenderStorage;
    class World;
}

namespace SceneUtil
{
    class WorkQueue;
}

namespace Resource
{
    class ResourceSystem;
}

namespace VFS
{
    class Manager;
}

namespace MWPhysics
{
    class PhysicsSystem;
}

namespace SceneUtil
{
    class WorkItem;
}

namespace MWWorld
{
    class Player;
    class CellStore;
    class CellPreloader;
    class World;

    enum class RotationOrder
    {
        direct,
        inverse
    };

    class Scene
    {
    public:
        using CellStoreCollection = std::set<CellStore*, std::less<>>;

    private:
        friend class World;

        struct ChangeCellGridRequest
        {
            osg::Vec3f mPosition;
            ESM::ExteriorCellLocation mCellIndex;
            bool mChangeEvent;
        };

        CellStore* mCurrentCell; // the cell the player is in
        CellStoreCollection mActiveCells;
        bool mCellChanged;
        bool mCellLoaded = false;
        MWWorld::World& mWorld;
        Render::FrameLifecycle& mFrameLifecycle;
        Render::SceneSynchronizer mSceneSynchronizer;
        Render::BonePoseResolver mBonePoseResolver;
        Render::MeshResolver mMeshResolver;
        Render::TextureResolver mTextureResolver;
        const VFS::Manager* mVfs;
        Resource::ResourceSystem* mResourceSystem;
        MWPhysics::PhysicsSystem* mPhysics;
        MWRender::RenderingManager& mRendering;
        MWRender::LandManager& mLandManager;
        Terrain::World*& mTerrain;
        MWRender::ObjectPaging*& mObjectPaging;
        osgUtil::IncrementalCompileOperation* mIncrementalCompileOperation;
        Terrain::RenderStorage& mTerrainStorage;
        SceneUtil::WorkQueue* mWorkQueue;
        DetourNavigator::Navigator& mNavigator;
        std::unique_ptr<CellPreloader> mPreloader;
        float mCellLoadingThreshold;
        float mPreloadDistance;
        bool mPreloadEnabled;

        bool mPreloadExteriorGrid;
        bool mPreloadDoors;
        bool mPreloadFastTravel;
        float mPredictionTime;
        float mLowestPoint;

        int mHalfGridSize = Constants::CellGridRadius;

        osg::Vec3f mLastPlayerPos;

        std::vector<ESM::RefNum> mPagedRefs;

        Render::WorldScene mNeutralWorldScene;
        bool mNeutralTerrainRegionsDirty = true;
        mutable std::unordered_map<std::string, std::weak_ptr<const std::vector<Render::MeshInstance>>>
            mNeutralMeshCache;

        std::vector<osg::ref_ptr<SceneUtil::WorkItem>> mWorkItems;

        std::optional<ChangeCellGridRequest> mChangeCellGridRequest;

        void insertCell(CellStore& cell, Loading::Listener* loadingListener,
            const DetourNavigator::UpdateGuard* navigatorUpdateGuard);

        std::array<int, 2> mCurrentGridCenter{};

        // Load and unload cells as necessary to create a cell grid with "X" and "Y" in the center
        void changeCellGrid(const osg::Vec3f& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent = true);

        void requestChangeCellGrid(
            const osg::Vec3f& position, const std::array<int, 2>& cell, bool changeEvent = true);

        void preloadCells(float dt);
        void preloadTeleportDoorDestinations(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos);
        void preloadExteriorGrid(const osg::Vec3f& playerPos, const osg::Vec3f& predictedPos);
        void preloadFastTravelDestinations(
            const osg::Vec3f& playerPos, std::vector<PositionCellGrid>& exteriorPositions);
        void preloadCellWithSurroundings(MWWorld::CellStore& cell);
        void preloadCell(MWWorld::CellStore& cell);
        void preloadTerrain(const osg::Vec3f& pos, ESM::RefId worldspace, bool sync = false);

        std::array<int, 4> gridCenterToBounds(const std::array<int, 2>& centerCell) const;
        std::array<int, 2> getNewGridCenter(
            const osg::Vec3f& pos, const std::array<int, 2>* currentGridCenter = nullptr) const;

        void unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard);
        void loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn, const osg::Vec3f& position,
            const DetourNavigator::UpdateGuard* navigatorUpdateGuard);
        void updateNeutralTerrainRegions();
        void synchronizeNeutralScene();

    public:
        Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
            Render::SceneSynchronizer sceneSynchronizer, Render::BonePoseResolver bonePoseResolver,
            Render::MeshResolver meshResolver, Render::TextureResolver textureResolver, const VFS::Manager* vfs,
            MWRender::RenderingManager& rendering, MWRender::LandManager& landManager,
            Terrain::World*& terrain, MWRender::ObjectPaging*& objectPaging,
            osgUtil::IncrementalCompileOperation* incrementalCompileOperation,
            Terrain::RenderStorage& terrainStorage, SceneUtil::WorkQueue* workQueue,
            Resource::ResourceSystem* resourceSystem,
            MWPhysics::PhysicsSystem* physics,
            DetourNavigator::Navigator& navigator);

        ~Scene();

        void reloadTerrain();

        void playerMoved(const osg::Vec3f& pos);

        void changePlayerCell(CellStore& newCell, const ESM::Position& position, bool adjustPlayerPos);

        CellStore* getCurrentCell();

        const CellStoreCollection& getActiveCells() const;

        bool hasCellChanged() const;
        ///< Has the set of active cells changed, since the last frame?

        bool hasCellLoaded() const { return mCellLoaded; }

        void resetCellLoaded() { mCellLoaded = false; }

        void changeToInteriorCell(
            std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent = true);
        ///< Move to interior cell.
        /// @param changeEvent Set cellChanged flag?

        void changeToExteriorCell(
            const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent = true);
        ///< Move to exterior cell.
        /// @param changeEvent Set cellChanged flag?

        void clear();
        ///< Change into a void

        /// Update renderer-neutral state through the world-scene owner.
        void updateNeutralObjectCell(const void* oldObjectKey, const void* newObjectKey, const void* cellKey,
            bool exterior, int gridX, int gridY, std::string_view cellName, std::string_view worldspace);
        void updateNeutralObjectPosition(const void* objectKey, const Render::Vec3& position);
        void updateNeutralObjectRotation(const void* objectKey, const Render::Quat& rotation);

        /// Export the current loaded-world state for a renderer backend.
        Render::SceneSubmission getNeutralScene();

    public:
        void markCellAsUnchanged();

        void update(float duration);

        void addObjectToScene(const Ptr& ptr);
        ///< Add an object that already exists in the world model to the scene.

        void removeObjectFromScene(const Ptr& ptr, bool keepActive = false);
        ///< Remove an object from the scene, but not from the world model.

        void addPostponedPhysicsObjects();

        void removeFromPagedRefs(const Ptr& ptr);

        bool isPagedRef(const Ptr& ptr) const;

        void updateObjectRotation(const Ptr& ptr, RotationOrder order);
        void updateObjectScale(const Ptr& ptr);

        bool isCellActive(const CellStore& cell);

        void preload(const std::string& mesh, bool useAnim = false);

        void testExteriorCells();
        void testInteriorCells();

        void reportStats(unsigned int frameNumber, osg::Stats& stats) const;
    };
}

#endif
