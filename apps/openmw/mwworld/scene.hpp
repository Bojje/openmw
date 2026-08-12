#ifndef GAME_MWWORLD_SCENE_H
#define GAME_MWWORLD_SCENE_H

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
#include <components/render/scene.hpp>
#include <components/render/submission.hpp>
#include <components/render/world.hpp>

namespace osg
{
    class Vec3f;
    class Stats;
}

namespace ESM
{
    struct Position;
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
    class ObjectPaging;
    class RenderingManager;
}

namespace Terrain
{
    class RenderStorage;
}

namespace VFS
{
    class Manager;
}

namespace MWPhysics
{
    class PhysicsSystem;
}

namespace MWWorld
{
    class CellStore;
    class CellPreloader;
    class WeatherManager;
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
        struct ChangeCellGridRequest
        {
            Render::Vec3 mPosition;
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
        Render::MeshResolver mMeshResolver;
        Render::TextureResolver mTextureResolver;
        Render::PoseResolver mPoseResolver;
        const VFS::Manager* mVfs;
        MWPhysics::PhysicsSystem* mPhysics;
        MWRender::RenderingManager* mRendering;
        MWRender::ObjectPaging* mObjectPaging;
        Terrain::RenderStorage& mTerrainStorage;
        DetourNavigator::Navigator& mNavigator;
        std::unique_ptr<CellPreloader> mPreloader;
        float mLowestPoint;

        int mHalfGridSize = Constants::CellGridRadius;

        Render::Vec3 mLastPlayerPos{};

        // Only the Vulkan scene owns renderer-neutral world state. The OSG
        // path uses its legacy scene graph directly and must not maintain a
        // second scene representation.
        std::unique_ptr<Render::WorldScene> mNeutralWorldScene;
        bool mNeutralTerrainRegionsDirty = true;
        mutable std::unordered_map<std::string, std::weak_ptr<const std::vector<Render::MeshInstance>>>
            mNeutralMeshCache;

        std::optional<ChangeCellGridRequest> mChangeCellGridRequest;

        void insertCell(CellStore& cell, Loading::Listener* loadingListener,
            const DetourNavigator::UpdateGuard* navigatorUpdateGuard);

        std::array<int, 2> mCurrentGridCenter{};

        // Load and unload cells as necessary to create a cell grid with "X" and "Y" in the center
        void changeCellGrid(const Render::Vec3& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent = true);

        void requestChangeCellGrid(
            const Render::Vec3& position, const std::array<int, 2>& cell, bool changeEvent = true);

        void preloadCells(float dt);
        void preloadTeleportDoorDestinations(const Render::Vec3& playerPos, const Render::Vec3& predictedPos);
        void preloadExteriorGrid(const Render::Vec3& playerPos, const Render::Vec3& predictedPos);
        void preloadFastTravelDestinations(
            const Render::Vec3& playerPos, std::vector<PositionCellGrid>& exteriorPositions);
        void preloadCellWithSurroundings(MWWorld::CellStore& cell);
        void preloadCell(MWWorld::CellStore& cell);
        void preloadTerrain(const Render::Vec3& pos, ESM::RefId worldspace, bool sync = false);

        std::array<int, 4> gridCenterToBounds(const std::array<int, 2>& centerCell) const;
        std::array<int, 2> getNewGridCenter(
            const Render::Vec3& pos, const std::array<int, 2>* currentGridCenter = nullptr) const;

        void unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard);
        void loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn,
            const DetourNavigator::UpdateGuard* navigatorUpdateGuard);
        void recordNeutralCell(CellStore& cell);
        void updateNeutralTerrainRegions();

    public:
        Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
            const VFS::Manager* vfs, MWRender::RenderingManager* rendering, MWRender::ObjectPaging* objectPaging,
            Terrain::RenderStorage& terrainStorage, std::unique_ptr<CellPreloader> preloader,
            MWPhysics::PhysicsSystem* physics, DetourNavigator::Navigator& navigator);

        Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
            Render::SceneSynchronizer sceneSynchronizer, Render::MeshResolver meshResolver,
            Render::TextureResolver textureResolver, Render::PoseResolver poseResolver, const VFS::Manager* vfs,
            MWRender::RenderingManager* rendering, MWRender::ObjectPaging* objectPaging,
            Terrain::RenderStorage& terrainStorage, std::unique_ptr<CellPreloader> preloader,
            MWPhysics::PhysicsSystem* physics,
            DetourNavigator::Navigator& navigator);

        Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
            Render::SceneSynchronizer sceneSynchronizer, Render::MeshResolver meshResolver,
            Render::TextureResolver textureResolver, Render::PoseResolver poseResolver, const VFS::Manager* vfs,
            Terrain::RenderStorage& terrainStorage,
            MWPhysics::PhysicsSystem* physics, DetourNavigator::Navigator& navigator);

        ~Scene();

        void reloadTerrain();

        void playerMoved(const Render::Vec3& pos);

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

    public:
        void markCellAsUnchanged();

        void update(float duration);

        void addObjectToScene(const Ptr& ptr);
        ///< Add an object that already exists in the world model to the scene.

        void removeObjectFromScene(const Ptr& ptr, bool keepActive = false);
        ///< Remove an object from the scene, but not from the world model.

        void addPostponedPhysicsObjects();

        void removeFromPagedRefs(const Ptr& ptr);

        void updateObjectRotation(const Ptr& ptr, RotationOrder order);
        void updateObjectScale(const Ptr& ptr);
        void updateObjectAnimation(const Ptr& ptr, std::string_view group,
            std::optional<float> animationTime = std::nullopt, std::string_view startKey = {},
            std::string_view stopKey = {});

        void updateNeutralObjectCell(const Ptr& oldPtr, const Ptr& newPtr);
        void updateNeutralObjectPosition(const Ptr& ptr, const Render::Vec3& position);
        void updateNeutralObjectRotation(const Ptr& ptr, const Render::Quat& rotation);

        void updateNeutralWaterLevel(float height);
        bool toggleNeutralWater();

        void recordNeutralEffect(std::string_view effectId, std::string_view model, const Render::Vec3& position,
            float scale, std::string_view textureOverride, bool loop, float animationDuration, bool isMagicVfx);
        void removeNeutralEffect(std::string_view effectId);
        void updateNeutralWeatherEffects(const WeatherManager& weatherManager);
        void clearNeutralWeatherEffects();

        bool isCellActive(const CellStore& cell);

        void preload(const std::string& mesh, bool useAnim = false);

        void testExteriorCells();
        void testInteriorCells();

        /// Export the current loaded-world state for the renderer-neutral frame owner.
        Render::SceneSubmission getNeutralScene();

        void reportStats(unsigned int frameNumber, osg::Stats& stats) const;
    };
}

#endif
