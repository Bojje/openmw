#include "scene.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>

#include <BulletCollision/CollisionDispatch/btCollisionObject.h>

#include <osg/Vec2i>
#include <osg/Vec3f>
#include <osg/Vec4i>

#include <components/debug/debuglog.hpp>
#include <components/detournavigator/agentbounds.hpp>
#include <components/detournavigator/debug.hpp>
#include <components/detournavigator/heightfieldshape.hpp>
#include <components/detournavigator/navigator.hpp>
#include <components/detournavigator/updateguard.hpp>
#include <components/esm/records.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/loadinglistener/loadinglistener.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/render/math.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/renderstorage.hpp>
#include <components/terrain/terraingrid.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/soundmanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/npcstats.hpp"

#include "../mwrender/landmanager.hpp"
#include "../mwrender/npcanimation.hpp"
#include "../mwrender/camera.hpp"
#include "../mwrender/objectpaging.hpp"
#include "../mwrender/postprocessor.hpp"
#include "../mwrender/renderingmanager.hpp"
#include "../mwrender/terrainstorage.hpp"

#include "../mwphysics/actor.hpp"
#include "../mwphysics/heightfield.hpp"
#include "../mwphysics/object.hpp"
#include "../mwphysics/physicssystem.hpp"

#include "../mwworld/actionteleport.hpp"

#include "cellpreloader.hpp"
#include "cellstore.hpp"
#include "cellvisitors.hpp"
#include "class.hpp"
#include "esmstore.hpp"
#include "localscripts.hpp"
#include "player.hpp"
#include "weather.hpp"
#include "worldimp.hpp"

namespace
{
    using MWWorld::RotationOrder;

    osg::Quat makeInversedOrderObjectOsgQuat(const ESM::Position& position)
    {
        const float xr = position.rot[0];
        const float yr = position.rot[1];
        const float zr = position.rot[2];

        return osg::Quat(xr, osg::Vec3(-1, 0, 0)) * osg::Quat(yr, osg::Vec3(0, -1, 0))
            * osg::Quat(zr, osg::Vec3(0, 0, -1));
    }

    osg::Quat makeInverseNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? Misc::Convert::makeActorOsgQuat(pos) : makeInversedOrderObjectOsgQuat(pos);
    }

    osg::Quat makeDirectNodeRotation(const MWWorld::Ptr& ptr)
    {
        const auto& pos = ptr.getRefData().getPosition();
        return ptr.getClass().isActor() ? Misc::Convert::makeActorOsgQuat(pos) : Misc::Convert::makeOsgQuat(pos);
    }

    osg::Quat makeNodeRotation(const MWWorld::Ptr& ptr, RotationOrder order)
    {
        if (order == RotationOrder::inverse)
            return makeInverseNodeRotation(ptr);
        return makeDirectNodeRotation(ptr);
    }

    Render::Quat makeDirectRenderRotation(const MWWorld::Ptr& ptr)
    {
        const auto& position = ptr.getRefData().getPosition();
        if (ptr.getClass().isActor())
            return Render::makeAxisAngleRotation({ 0.f, 0.f, -1.f }, position.rot[2]);
        return Render::makeEulerRotation({ position.rot[0], position.rot[1], position.rot[2] });
    }

    MWWorld::PositionCellGrid makeTerrainPreloadPosition(
        const Render::Vec3& position, const std::array<int, 4>& bounds)
    {
        return { { position.x, position.y, position.z },
            bounds };
    }

    void recordNeutralObject(const MWWorld::World& world, const MWWorld::Ptr& ptr, std::string_view model,
        bool visible, Render::WorldScene* neutralWorld)
    {
        if (neutralWorld == nullptr)
            return;
        if (ptr.isEmpty() || model.empty())
        {
            if (!ptr.isEmpty())
                neutralWorld->removeObject(static_cast<const void*>(ptr.mRef));
            return;
        }

        const MWWorld::CellStore* cell = ptr.getCell();
        const auto& position = ptr.getRefData().getPosition();
        Render::Vec3 scale{ ptr.getCellRef().getScale(), ptr.getCellRef().getScale(), ptr.getCellRef().getScale() };
        ptr.getClass().adjustScale(ptr, scale, true);

        Render::ObjectTransform transform;
        transform.position = { position.pos[0], position.pos[1], position.pos[2] };
        transform.rotation = makeDirectRenderRotation(ptr);
        transform.scale = scale;
        const std::vector<VFS::Path::Normalized> sourcePaths = world.getNeutralAnimationSources(ptr);
        std::vector<std::string> animationSources;
        animationSources.reserve(sourcePaths.size());
        for (const VFS::Path::Normalized& source : sourcePaths)
            animationSources.push_back(source.value());
        neutralWorld->recordObject(static_cast<const void*>(ptr.mRef), static_cast<const void*>(cell),
            cell->getCell()->isExterior(), cell->getCell()->getGridX(), cell->getCell()->getGridY(),
            cell->getCell()->getNameId(), model, transform, visible, cell->getCell()->getWorldSpace().serializeText(),
            ptr.getClass().useAnim(), animationSources);

        if (!ptr.getClass().isNpc() || !ptr.getClass().useAnim())
            return;

        const ESM::NPC* npc = ptr.get<ESM::NPC>()->mBase;
        const ESM::Race* race = world.getStore().get<ESM::Race>().find(npc->mRace);
        if (race == nullptr)
            return;
        const bool werewolf = ptr.getClass().getNpcStats(ptr).isWerewolf();
        const auto& bodyParts
            = MWRender::NpcAnimation::getBodyParts(npc->mRace, !npc->isMale(), false, werewolf);
        static constexpr std::array<std::string_view, ESM::PRT_Count> bones = { "Head", "Head", "Neck", "Chest",
            "Groin", "Groin", "Right Hand", "Left Hand", "Right Wrist", "Left Wrist", "Shield Bone",
            "Right Forearm", "Left Forearm", "Right Upper Arm", "Left Upper Arm", "Right Foot", "Left Foot",
            "Right Ankle", "Left Ankle", "Right Knee", "Left Knee", "Right Upper Leg", "Left Upper Leg",
            "Right Clavicle", "Left Clavicle", "Weapon Bone", "Tail" };
        for (int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
            neutralWorld->updateObjectAttachment(static_cast<const void*>(ptr.mRef),
                "bodypart-" + std::to_string(part), {}, {}, false);
        neutralWorld->updateObjectAttachment(static_cast<const void*>(ptr.mRef), "head", {}, {}, false);
        neutralWorld->updateObjectAttachment(static_cast<const void*>(ptr.mRef), "hair", {}, {}, false);
        for (int part = ESM::PRT_Neck; part < ESM::PRT_Count; ++part)
        {
            if (part >= static_cast<int>(bodyParts.size()) || bodyParts[part] == nullptr)
                continue;
            const VFS::Path::Normalized partModel
                = Misc::ResourceHelpers::correctMeshPath(bodyParts[part]->mModel.getNormalized());
            if (!partModel.empty())
                neutralWorld->updateObjectAttachment(static_cast<const void*>(ptr.mRef),
                    "bodypart-" + std::to_string(part), partModel.value(), bones[part], true);
        }

        const auto addNamedPart = [&](std::string_view id, const ESM::RefId& name) {
            if (name.empty())
                return;
            const ESM::BodyPart* bodyPart = world.getStore().get<ESM::BodyPart>().search(name);
            if (bodyPart == nullptr)
                return;
            const VFS::Path::Normalized partModel
                = Misc::ResourceHelpers::correctMeshPath(bodyPart->mModel.getNormalized());
            if (!partModel.empty())
                neutralWorld->updateObjectAttachment(static_cast<const void*>(ptr.mRef), id, partModel.value(), "Head", true);
        };
        addNamedPart("head", npc->mHead);
        addNamedPart("hair", npc->mHair);
    }

    void setNodeRotation(const MWWorld::Ptr& ptr, MWRender::RenderingManager& rendering, const osg::Quat& rotation)
    {
        if (ptr.getRefData().getBaseNode())
            rendering.rotateObject(ptr, rotation);
    }

    VFS::Path::Normalized getModel(const MWWorld::Ptr& ptr)
    {
        if (Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return {};
        return ptr.getClass().getCorrectedModel(ptr);
    }

    // Null node meant to distinguish objects that aren't in the scene from paged objects
    // TODO: find a more clever way to make paging exclusion more reliable?
    static osg::ref_ptr<SceneUtil::PositionAttitudeTransform> pagedNode = new SceneUtil::PositionAttitudeTransform;

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, MWRender::ObjectPaging* objectPaging,
        MWPhysics::PhysicsSystem& physics, MWRender::RenderingManager* rendering, Render::WorldScene* neutralWorld)
    {
        if (ptr.getRefData().getBaseNode() || physics.getActor(ptr))
        {
            Log(Debug::Warning) << "Warning: Tried to add " << ptr.getCellRef().getRefId() << " to the scene twice";
            return;
        }

        const VFS::Path::Normalized model = getModel(ptr);
        const auto rotation = makeDirectNodeRotation(ptr);

        if (!model.empty())
        {
            // Record neutral ownership before the optional legacy scene graph
            // is touched. A paging or OSG insertion failure must not erase
            // the backend's active-cell snapshot.
            recordNeutralObject(world, ptr, model.view(), true, neutralWorld);
        }

        if (rendering)
        {
            ESM::RefNum refnum = ptr.getCellRef().getRefNum();
            const bool isPaged = objectPaging && refnum.hasContentFile() && objectPaging->isPagedRef(refnum);
            if (!isPaged)
                ptr.getClass().insertObjectRendering(ptr, model, rendering->getObjects());
            else
                ptr.getRefData().setBaseNode(pagedNode);
            setNodeRotation(ptr, *rendering, rotation);
        }

        if (ptr.getClass().useAnim())
            MWBase::Environment::get().getMechanicsManager()->add(ptr);

        if (ptr.getClass().isActor() && rendering)
            rendering->addWaterRippleEmitter(ptr);

        // Restore effect particles
        world.applyLoopingParticles(ptr);

        if (!model.empty())
            ptr.getClass().insertObject(ptr, model, rotation, physics);

        MWBase::Environment::get().getLuaManager()->objectAddedToScene(ptr);
    }

    void addObject(const MWWorld::Ptr& ptr, const MWWorld::World& world, const MWPhysics::PhysicsSystem& physics,
        float& lowestPoint, bool isInterior, DetourNavigator::Navigator& navigator,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard = nullptr)
    {
        if (const auto object = physics.getObject(ptr))
        {
            // Find the lowest point of this collision object in world space from its AABB if interior
            // this point is used to determine the infinite fall cutoff from lowest point in the cell
            if (isInterior)
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                const auto transform = object->getTransform();
                object->getShapeInstance()->mCollisionShape->getAabb(transform, aabbMin, aabbMax);
                lowestPoint = std::min(lowestPoint, static_cast<float>(aabbMin.z()));
            }

            const DetourNavigator::ObjectTransform objectTransform{ ptr.getRefData().getPosition(),
                ptr.getCellRef().getScale() };

            if (ptr.getClass().isDoor() && !ptr.getCellRef().getTeleport())
            {
                btVector3 aabbMin;
                btVector3 aabbMax;
                object->getShapeInstance()->mCollisionShape->getAabb(btTransform::getIdentity(), aabbMin, aabbMax);

                const auto center = (aabbMax + aabbMin) * 0.5f;

                const auto distanceFromDoor = world.getMaxActivationDistance() * 0.5f;
                const auto toPoint = aabbMax.x() - aabbMin.x() < aabbMax.y() - aabbMin.y()
                    ? btVector3(distanceFromDoor, 0, 0)
                    : btVector3(0, distanceFromDoor, 0);

                const auto transform = object->getTransform();
                const btTransform closedDoorTransform(
                    Misc::Convert::makeBulletQuaternion(ptr.getCellRef().getPosition()), transform.getOrigin());

                const auto start = Misc::Convert::makeOsgVec3f(closedDoorTransform(center + toPoint));
                const auto startPoint = physics.castRay(start, start - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionStart = startPoint.mHit ? startPoint.mHitPos : start;

                const auto end = Misc::Convert::makeOsgVec3f(closedDoorTransform(center - toPoint));
                const auto endPoint = physics.castRay(end, end - osg::Vec3f(0, 0, 1000), { ptr }, {},
                    MWPhysics::CollisionType_World | MWPhysics::CollisionType_HeightMap
                        | MWPhysics::CollisionType_Water);
                const auto connectionEnd = endPoint.mHit ? endPoint.mHitPos : end;

                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::DoorShapes(
                        object->getShapeInstance(), objectTransform, connectionStart, connectionEnd),
                    transform, navigatorUpdateGuard);
            }
            else if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
            {
                navigator.addObject(DetourNavigator::ObjectId(object),
                    DetourNavigator::ObjectShapes(object->getShapeInstance(), objectTransform), object->getTransform(),
                    navigatorUpdateGuard);
            }
        }
        else if (physics.getActor(ptr))
        {
            const DetourNavigator::AgentBounds agentBounds = world.getPathfindingAgentBounds(ptr);
            if (!navigator.addAgent(agentBounds))
                Log(Debug::Warning) << "Agent bounds are not supported by navigator for " << ptr.toString() << ": "
                                    << agentBounds;
        }
    }

    struct InsertVisitor
    {
        MWWorld::CellStore& mCell;
        Loading::Listener* mLoadingListener;

        std::vector<MWWorld::Ptr> mToInsert;

        InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener);

        bool operator()(const MWWorld::Ptr& ptr);

        template <class AddObject>
        void insert(AddObject&& addObject);
    };

    InsertVisitor::InsertVisitor(MWWorld::CellStore& cell, Loading::Listener* loadingListener)
        : mCell(cell)
        , mLoadingListener(loadingListener)
    {
    }

    bool InsertVisitor::operator()(const MWWorld::Ptr& ptr)
    {
        // do not insert directly as we can't modify the cell from within the visitation
        // CreatureLevList::insertObjectRendering may spawn a new creature
        mToInsert.push_back(ptr);
        return true;
    }

    template <class AddObject>
    void InsertVisitor::insert(AddObject&& addObject)
    {
        for (MWWorld::Ptr& ptr : mToInsert)
        {
            if (!ptr.mRef->isDeleted() && ptr.getRefData().isEnabled())
            {
                try
                {
                    addObject(ptr);
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
                }
            }

            if (mLoadingListener != nullptr)
                mLoadingListener->increaseProgress(1);
        }
    }

    int getCellPositionDistanceToOrigin(const std::pair<int, int>& cellPosition)
    {
        return std::abs(cellPosition.first) + std::abs(cellPosition.second);
    }

    bool isCellInCollection(ESM::ExteriorCellLocation cellIndex, MWWorld::Scene::CellStoreCollection& collection)
    {
        for (auto* cell : collection)
        {
            assert(cell->getCell()->isExterior());
            if (cellIndex == cell->getCell()->getExteriorCellLocation())
                return true;
        }
        return false;
    }

    template <class Function>
    void iterateOverCellsAround(int cellX, int cellY, int range, Function&& f)
    {
        for (int x = cellX - range, lastX = cellX + range; x <= lastX; ++x)
            for (int y = cellY - range, lastY = cellY + range; y <= lastY; ++y)
                f(x, y);
    }

    void sortCellsToLoad(int centerX, int centerY, std::vector<std::pair<int, int>>& cells)
    {
        const auto getDistanceToPlayerCell = [&](const std::pair<int, int>& cellPosition) {
            return std::abs(cellPosition.first - centerX) + std::abs(cellPosition.second - centerY);
        };

        const auto getCellPositionPriority = [&](const std::pair<int, int>& cellPosition) {
            return std::make_pair(getDistanceToPlayerCell(cellPosition), getCellPositionDistanceToOrigin(cellPosition));
        };

        std::sort(cells.begin(), cells.end(), [&](const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) {
            return getCellPositionPriority(lhs) < getCellPositionPriority(rhs);
        });
    }
}

namespace MWWorld
{
    void Scene::removeFromPagedRefs(const Ptr& ptr)
    {
        if (!mRendering)
            return;

        ESM::RefNum refnum = ptr.getCellRef().getRefNum();
        if (mObjectPaging && refnum.hasContentFile() && mObjectPaging->isPagedRef(refnum))
        {
            mObjectPaging->removePagedRef(refnum);
            if (!ptr.getRefData().getBaseNode())
                return;
            const VFS::Path::Normalized model = getModel(ptr);
            ptr.getClass().insertObjectRendering(ptr, model, mRendering->getObjects());
            setNodeRotation(ptr, *mRendering, makeNodeRotation(ptr, RotationOrder::direct));
            recordNeutralObject(mWorld, ptr, model.view(), true, mNeutralWorldScene.get());
            reloadTerrain();
        }
    }

    void Scene::updateObjectRotation(const Ptr& ptr, RotationOrder order)
    {
        const auto rot = makeNodeRotation(ptr, order);
        if (mRendering)
            setNodeRotation(ptr, *mRendering, rot);
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectRotation(static_cast<const void*>(ptr.mRef), makeDirectRenderRotation(ptr));
        mPhysics->updateRotation(ptr, rot);
    }

    void Scene::updateObjectScale(const Ptr& ptr)
    {
        float scale = ptr.getCellRef().getScale();
        Render::Vec3 scaleVec{ scale, scale, scale };
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        if (mRendering)
            mRendering->scaleObject(ptr, osg::Vec3f(scaleVec.x, scaleVec.y, scaleVec.z));
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectScale(static_cast<const void*>(ptr.mRef), scaleVec);
        mPhysics->updateScale(ptr);
    }

    void Scene::updateObjectAnimation(const Ptr& ptr, std::string_view group, std::optional<float> animationTime,
        std::string_view startKey, std::string_view stopKey, bool looping)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectAnimation(
                static_cast<const void*>(ptr.mRef), group, animationTime, startKey, stopKey, looping);
    }

    void Scene::updateNeutralObjectAttachment(
        const Ptr& ptr, std::string_view attachmentId, std::string_view model, std::string_view bone, bool visible)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectAttachment(
                static_cast<const void*>(ptr.mRef), attachmentId, model, bone, visible);
    }

    bool Scene::isNeutralObjectAnimationPlaying(const Ptr& ptr, std::string_view group, float duration) const
    {
        return mNeutralWorldScene
            && mNeutralWorldScene->isObjectAnimationPlaying(static_cast<const void*>(ptr.mRef), group, duration);
    }

    void Scene::updateNeutralObjectCell(const Ptr& oldPtr, const Ptr& newPtr)
    {
        if (!mNeutralWorldScene || oldPtr.isEmpty() || newPtr.isEmpty())
            return;

        const CellStore* destinationCell = newPtr.getCell();
        mNeutralWorldScene->updateObjectCell(static_cast<const void*>(oldPtr.mRef), static_cast<const void*>(newPtr.mRef),
            static_cast<const void*>(destinationCell), destinationCell->getCell()->isExterior(),
            destinationCell->getCell()->getGridX(), destinationCell->getCell()->getGridY(),
            destinationCell->getCell()->getNameId(), destinationCell->getCell()->getWorldSpace().serializeText());
    }

    void Scene::updateNeutralObjectPosition(const Ptr& ptr, const Render::Vec3& position)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectPosition(static_cast<const void*>(ptr.mRef), position);
    }

    void Scene::updateNeutralObjectRotation(const Ptr& ptr, const Render::Quat& rotation)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectRotation(static_cast<const void*>(ptr.mRef), rotation);
    }

    void Scene::updateNeutralObjectVisibility(const Ptr& ptr, float visibility)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectVisibility(static_cast<const void*>(ptr.mRef), visibility);
    }

    void Scene::updateNeutralObjectActive(const Ptr& ptr, bool active)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateObjectActive(static_cast<const void*>(ptr.mRef), active);
    }

    void Scene::emitNeutralWaterRipple(const Render::Vec3& position, float size)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->emitWaterRipple(position, size);
    }

    void Scene::updateNeutralWaterLevel(float height)
    {
        if (mNeutralWorldScene && mCurrentCell)
            mNeutralWorldScene->updateWaterLevel(static_cast<const void*>(mCurrentCell), height);
    }

    bool Scene::toggleNeutralWater()
    {
        if (!mNeutralWorldScene)
            return false;
        mNeutralWorldScene->setWaterEnabled(!mNeutralWorldScene->waterEnabled());
        return mNeutralWorldScene->waterEnabled();
    }

    void Scene::recordNeutralEffect(std::string_view effectId, std::string_view model, const Render::Vec3& position,
        float scale, std::string_view textureOverride, bool loop, float animationDuration, bool isMagicVfx,
        bool ambientOverride, const Render::Vec4& pointLightColor, float pointLightRadius,
        const Render::Vec4& emissiveColor, bool emissiveOverride, std::span<const std::string> additionalModels)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->recordEffect(
                effectId, model, position, scale, textureOverride, loop, animationDuration, isMagicVfx,
                ambientOverride, pointLightColor, pointLightRadius, emissiveColor, emissiveOverride, additionalModels);
    }

    void Scene::removeNeutralEffect(std::string_view effectId)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->removeEffect(effectId);
    }

    void Scene::updateNeutralEffect(std::string_view effectId, const Render::Vec3& position, const Render::Quat& rotation)
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateEffect(effectId, position, rotation);
    }

    void Scene::updateNeutralWeatherEffects(const WeatherManager& weatherManager)
    {
        if (mNeutralWorldScene)
            weatherManager.updateNeutralWeatherEffects(*mNeutralWorldScene);
    }

    void Scene::clearNeutralWeatherEffects()
    {
        if (mNeutralWorldScene)
            mNeutralWorldScene->clearWeatherEffects();
    }

    void Scene::update(float duration)
    {
        if (mChangeCellGridRequest.has_value())
        {
            changeCellGrid(mChangeCellGridRequest->mPosition, mChangeCellGridRequest->mCellIndex,
                mChangeCellGridRequest->mChangeEvent);
            mChangeCellGridRequest.reset();
        }

        if (mPreloader)
            mPreloader->updateCache(mFrameLifecycle.referenceTime());
        if (mNeutralWorldScene)
            mNeutralWorldScene->updateEffects(duration);
        preloadCells(duration);
    }

    void Scene::unloadCell(CellStore* cell, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        if (mActiveCells.find(cell) == mActiveCells.end())
            return;
        Log(Debug::Info) << "Unloading cell " << cell->getCell()->getDescription();

        ListAndResetObjectsVisitor visitor;

        cell->forEach(visitor, true); // Include objects being teleported by Lua
        for (const auto& ptr : visitor.mObjects)
        {
            if (const auto object = mPhysics->getObject(ptr))
            {
                if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                    mNavigator.removeObject(DetourNavigator::ObjectId(object), navigatorUpdateGuard);
                mPhysics->remove(ptr);
            }
            else if (mPhysics->getActor(ptr))
            {
                mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
                if (mRendering)
                    mRendering->removeActorPath(ptr);
                mPhysics->remove(ptr);
            }
            else
                ptr.mRef->mData.mPhysicsPostponed = false;
            MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        }

        const auto cellX = cell->getCell()->getGridX();
        const auto cellY = cell->getCell()->getGridY();

        if (cell->getCell()->isExterior())
        {
            mNavigator.removeHeightfield(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);
            mPhysics->removeHeightField(cellX, cellY);
        }

        if (cell->getCell()->hasWater())
            mNavigator.removeWater(osg::Vec2i(cellX, cellY), navigatorUpdateGuard);

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.removePathgrid(*pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell->getCell());

        MWBase::Environment::get().getMechanicsManager()->drop(cell);

        if (mNeutralWorldScene)
            mNeutralWorldScene->removeCell(static_cast<const void*>(cell));
        if (mRendering)
            mRendering->removeCell(cell);
        if (mRendering)
            MWBase::Environment::get().getWindowManager()->removeCell(cell);

        mWorld.getLocalScripts().clearCell(cell);

        if (mRendering)
            MWBase::Environment::get().getSoundManager()->stopSound(cell);
        mActiveCells.erase(cell);
        mNeutralTerrainRegionsDirty = true;
        // Clean up any effects that may have been spawned while unloading all cells
        if (mActiveCells.empty())
        {
            if (mNeutralWorldScene)
                mNeutralWorldScene->clearEffects();
            if (mRendering)
                mRendering->notifyWorldSpaceChanged();
        }
    }

    void Scene::recordNeutralCell(CellStore& cell)
    {
        if (!mNeutralWorldScene)
            return;

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();
        const MWWorld::Cell& cellVariant = *cell.getCell();
        ESM::RefId worldspace = cellVariant.getWorldSpace();
        std::optional<Render::WaterSurface> water;
        if (cellVariant.isExterior())
        {
            const float cellSize = static_cast<float>(Constants::CellSizeInUnits);
            water = Render::WaterSurface{ cellX * cellSize, (cellX + 1) * cellSize,
                cellY * cellSize, (cellY + 1) * cellSize, cell.getWaterLevel() };
        }
        mNeutralWorldScene->recordCell(static_cast<const void*>(&cell), cellVariant.isExterior(), cellX,
            cellY, cellVariant.getNameId(), worldspace.serializeText(), std::move(water));

        if (cellVariant.isExterior() && Settings::groundcover().mEnabled
            && worldspace == ESM::Cell::sDefaultWorldspaceId)
        {
            for (const GroundcoverRecord& record : mWorld.getGroundcoverStore().getCellRecords(
                     cellX, cellY, Settings::groundcover().mDensity))
            {
                Render::ObjectTransform transform;
                transform.position = { record.position.pos[0], record.position.pos[1], record.position.pos[2] };
                transform.rotation = Render::makeEulerRotation(
                    { record.position.rot[0], record.position.rot[1], record.position.rot[2] });
                transform.scale = { record.scale, record.scale, record.scale };
                mNeutralWorldScene->recordStaticObject(static_cast<const void*>(&cell), true, cellX, cellY,
                    cellVariant.getNameId(), record.model.value(), transform, true, worldspace.serializeText());
            }
        }

        if (cellVariant.isExterior())
            mNeutralWorldScene->setTerrainTiles(static_cast<const void*>(&cell),
                mTerrainStorage.getRenderTiles(cellX, cellY, worldspace));
    }

    void Scene::loadCell(CellStore& cell, Loading::Listener* loadingListener, bool respawn,
        const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        using DetourNavigator::HeightfieldShape;

        assert(mActiveCells.find(&cell) == mActiveCells.end());
        mActiveCells.insert(&cell);

        Log(Debug::Info) << "Loading cell " << cell.getCell()->getDescription();

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();
        const MWWorld::Cell& cellVariant = *cell.getCell();
        ESM::RefId worldspace = cellVariant.getWorldSpace();
        recordNeutralCell(cell);

        if (cellVariant.isExterior())
        {
            const int verts = ESM::getLandSize(worldspace);
            const int worldsize = ESM::getCellSize(worldspace);
            bool hasTerrainData = false;

            if (std::optional<Render::TerrainHeightField> heightField
                = mTerrainStorage.getHeightField(cellX, cellY, worldspace);
                heightField && heightField->valid())
            {
                hasTerrainData = true;
                mPhysics->addHeightField(std::move(heightField->heights), cellX, cellY, worldsize, verts,
                    heightField->minHeight, heightField->maxHeight);
            }
            else if (!ESM::isEsm4Ext(worldspace))
            {
                static const std::vector<float> defaultHeight(verts * verts, ESM::Land::DEFAULT_HEIGHT);
                mPhysics->addHeightField(defaultHeight, cellX, cellY, worldsize, verts,
                    ESM::Land::DEFAULT_HEIGHT, ESM::Land::DEFAULT_HEIGHT);
            }
            if (mPhysics->getHeightField(cellX, cellY))
            {
                const osg::Vec2i cellPosition(cellX, cellY);
                const HeightfieldShape shape = [&]() -> HeightfieldShape {
                    if (!hasTerrainData)
                    {
                        return DetourNavigator::HeightfieldPlane{ static_cast<float>(ESM::Land::DEFAULT_HEIGHT) };
                    }
                    else
                    {
                        const MWPhysics::HeightField* field = mPhysics->getHeightField(cellX, cellY);
                        DetourNavigator::HeightfieldSurface heights;
                        heights.mHeights = field->getHeights();
                        heights.mSize = field->getVertexCount();
                        heights.mMinHeight = field->getMinHeight();
                        heights.mMaxHeight = field->getMaxHeight();
                        return heights;
                    }
                }();
                mNavigator.addHeightfield(cellPosition, worldsize, shape, navigatorUpdateGuard);
            }
        }

        ESM::visit(ESM::VisitOverload{
                       [&](const ESM::Cell& c) {
                           if (const auto pathgrid = mWorld.getStore().get<ESM::Pathgrid>().search(c))
                               mNavigator.addPathgrid(c, *pathgrid);
                       },
                       [&](const ESM4::Cell& /*c*/) {},
                   },
            *cell.getCell());

        // register local scripts
        // do this before insertCell, to make sure we don't add scripts from levelled creature spawning twice
        mWorld.getLocalScripts().addCell(&cell);

        if (respawn)
            cell.respawn();

        insertCell(cell, loadingListener, navigatorUpdateGuard);

        if (mRendering)
            mRendering->addCell(&cell);
        mNeutralTerrainRegionsDirty = true;

        if (mRendering)
            MWBase::Environment::get().getWindowManager()->addCell(&cell);
        bool waterEnabled = cellVariant.hasWater() || cell.isExterior();
        float waterLevel = cell.getWaterLevel();
        if (mRendering)
            mRendering->setWaterEnabled(waterEnabled);
        if (waterEnabled)
        {
            mPhysics->enableWater(waterLevel);
            if (mRendering)
                mRendering->setWaterHeight(waterLevel);

            if (cellVariant.isExterior())
            {
                if (mPhysics->getHeightField(cellX, cellY))
                    mNavigator.addWater(
                        osg::Vec2i(cellX, cellY), ESM::Land::REAL_SIZE, waterLevel, navigatorUpdateGuard);
            }
            else
            {
                mNavigator.addWater(
                    osg::Vec2i(cellX, cellY), std::numeric_limits<int>::max(), waterLevel, navigatorUpdateGuard);
            }
        }
        else
            mPhysics->disableWater();

        if (!cell.isExterior() && !cellVariant.isQuasiExterior())
            if (mRendering)
                mRendering->configureAmbient(cellVariant);

        if (mPreloader)
            mPreloader->notifyLoaded(&cell);
    }

    void Scene::clear()
    {
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            unloadCell(cell, navigatorUpdateGuard.get());
        }
        navigatorUpdateGuard.reset();
        assert(mActiveCells.empty());
        if (mNeutralWorldScene)
            mNeutralWorldScene->clear();
        mNeutralTerrainRegionsDirty = false;
        if (mNeutralWorldScene)
            mNeutralMeshCache.clear();
        mCurrentCell = nullptr;
        mLowestPoint = std::numeric_limits<float>::max();

        if (mPreloader)
            mPreloader->clear();
    }

    std::array<int, 4> Scene::gridCenterToBounds(const std::array<int, 2>& centerCell) const
    {
        return { centerCell[0] - mHalfGridSize, centerCell[1] - mHalfGridSize,
            centerCell[0] + mHalfGridSize + 1, centerCell[1] + mHalfGridSize + 1 };
    }

    std::array<int, 2> Scene::getNewGridCenter(
        const Render::Vec3& pos, const std::array<int, 2>* currentGridCenter) const
    {
        ESM::RefId worldspace
            = mCurrentCell ? mCurrentCell->getCell()->getWorldSpace() : ESM::Cell::sDefaultWorldspaceId;
        if (currentGridCenter)
        {
            const osg::Vec2f center = ESM::indexToPosition(
                ESM::ExteriorCellLocation((*currentGridCenter)[0], (*currentGridCenter)[1], worldspace), true);
            float distance = std::max(std::abs(center.x() - pos.x), std::abs(center.y() - pos.y));
            int cellSize = ESM::getCellSize(worldspace);
            constexpr float cellLoadingThreshold = 1024.f;
            const float maxDistance = cellSize / 2 + cellLoadingThreshold; // 1/2 cell size + threshold
            if (distance <= maxDistance)
                return *currentGridCenter;
        }
        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x, pos.y, worldspace);
        return { cellPos.mX, cellPos.mY };
    }

    void Scene::playerMoved(const Render::Vec3& pos)
    {
        if (!mCurrentCell)
            return;

        // The player is reset when z is 90 units below the lowest reference bound z.
        constexpr float lowestPointAdjustment = -90.0f;
        if (mCurrentCell->isExterior())
        {
            const std::array<int, 2> newCell = getNewGridCenter(pos, &mCurrentGridCenter);
            if (newCell != mCurrentGridCenter)
                requestChangeCellGrid(pos, newCell);
        }
        else if (pos.z < mLowestPoint + lowestPointAdjustment)
        {
            // Player has fallen into the void, reset to interior marker/coc (#1415)
            const std::string_view cellNameId = mCurrentCell->getCell()->getNameId();
            MWBase::World* world = MWBase::Environment::get().getWorld();
            MWWorld::Ptr playerPtr = world->getPlayerPtr();

            // Check that collision is enabled, which is opposite to Vanilla
            // this change was decided in MR #4100 as the behaviour is preferable
            if (world->isActorCollisionEnabled(playerPtr))
            {
                ESM::Position newPos;
                const ESM::RefId refId = world->findInteriorPosition(cellNameId, newPos);

                // Only teleport if that teleport point is > the lowest point, rare edge case
                if (!refId.empty() && newPos.pos[2] >= mLowestPoint - lowestPointAdjustment)
                {
                    MWWorld::ActionTeleport(refId, newPos, false).execute(playerPtr);
                    Log(Debug::Warning) << "Player position has been reset due to falling into the void";
                }
            }
        }
    }

    void Scene::requestChangeCellGrid(const Render::Vec3& position, const std::array<int, 2>& cell, bool changeEvent)
    {
        mChangeCellGridRequest = ChangeCellGridRequest{ position,
            ESM::ExteriorCellLocation(cell[0], cell[1], mCurrentCell->getCell()->getWorldSpace()), changeEvent };
    }

    void Scene::changeCellGrid(const Render::Vec3& pos, ESM::ExteriorCellLocation playerCellIndex, bool changeEvent)
    {
        const int halfGridSize
            = isEsm4Ext(playerCellIndex.mWorldspace) ? Constants::ESM4CellGridRadius : Constants::CellGridRadius;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        const int playerCellX = playerCellIndex.mX;
        const int playerCellY = playerCellIndex.mY;

        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cell = *iter++;
            if (cell->getCell()->isExterior() && cell->getCell()->getWorldSpace() == playerCellIndex.mWorldspace)
            {
                const auto dx = std::abs(playerCellX - cell->getCell()->getGridX());
                const auto dy = std::abs(playerCellY - cell->getCell()->getGridY());
                if (dx > halfGridSize || dy > halfGridSize)
                    unloadCell(cell, navigatorUpdateGuard.get());
            }
            else
                unloadCell(cell, navigatorUpdateGuard.get());
        }

        const DetourNavigator::CellGridBounds cellGridBounds{
            .mCenter = osg::Vec2i(playerCellX, playerCellY),
            .mHalfSize = halfGridSize,
        };

        mNavigator.updateBounds(playerCellIndex.mWorldspace, cellGridBounds,
            osg::Vec3f(pos.x, pos.y, pos.z), navigatorUpdateGuard.get());

        mHalfGridSize = halfGridSize;
        mCurrentGridCenter = { playerCellX, playerCellY };
        const std::array<int, 4> newGrid = gridCenterToBounds(mCurrentGridCenter);

        // NOTE: setActiveGrid must be after enableTerrain, otherwise we set the grid in the old exterior worldspace
        if (mRendering)
        {
            mRendering->enableTerrain(true, playerCellIndex.mWorldspace);
            mRendering->setActiveGrid(osg::Vec4i(newGrid[0], newGrid[1], newGrid[2], newGrid[3]));
        }

        if (mPreloader && mObjectPaging && mObjectPaging->unlockCache())
        {
            mPreloader->rebuildTerrainViews();
            mPreloader->abortTerrainPreloadExcept(nullptr);
        }
        if (mPreloader && !mPreloader->isTerrainLoaded(
                makeTerrainPreloadPosition(pos, newGrid), mFrameLifecycle.referenceTime()))
            preloadTerrain(pos, playerCellIndex.mWorldspace, true);
        if (mObjectPaging && mRendering)
            mObjectPaging->updatePagedRefs(osg::Vec4i(newGrid[0], newGrid[1], newGrid[2], newGrid[3]));

        addPostponedPhysicsObjects();

        std::size_t refsToLoad = 0;
        std::vector<std::pair<int, int>> cellsPositionsToLoad;
        iterateOverCellsAround(playerCellX, playerCellY, mHalfGridSize, [&](int x, int y) {
            const ESM::ExteriorCellLocation location(x, y, playerCellIndex.mWorldspace);
            if (isCellInCollection(location, mActiveCells))
                return;
            refsToLoad += mWorld.getWorldModel().getExterior(location).count();
            cellsPositionsToLoad.emplace_back(x, y);
        });

        Loading::Listener* loadingListener
            = mRendering ? MWBase::Environment::get().getWindowManager()->getLoadingScreen() : nullptr;
        Loading::ScopedLoad load(loadingListener);
        if (loadingListener)
        {
            loadingListener->setLabel("#{OMWEngine:LoadingExterior}");
            loadingListener->setProgressRange(refsToLoad);
        }

        sortCellsToLoad(playerCellX, playerCellY, cellsPositionsToLoad);

        for (const auto& [x, y] : cellsPositionsToLoad)
        {
            ESM::ExteriorCellLocation indexToLoad = { x, y, playerCellIndex.mWorldspace };
            if (!isCellInCollection(indexToLoad, mActiveCells))
            {
                CellStore& cell = mWorld.getWorldModel().getExterior(indexToLoad);
                loadCell(cell, loadingListener, changeEvent, navigatorUpdateGuard.get());
            }
        }

        mNavigator.update(osg::Vec3f(pos.x, pos.y, pos.z), navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        CellStore& current = mWorld.getWorldModel().getExterior(playerCellIndex);
        if (mRendering)
            MWBase::Environment::get().getWindowManager()->changeCell(&current);

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;
    }

    void Scene::addPostponedPhysicsObjects()
    {
        for (const auto& cell : mActiveCells)
        {
            cell->forEach([&](const MWWorld::Ptr& ptr) {
                if (ptr.mRef->mData.mPhysicsPostponed)
                {
                    ptr.mRef->mData.mPhysicsPostponed = false;
                    if (ptr.mRef->mData.isEnabled() && ptr.mRef->mRef.getCount() > 0)
                    {
                        const VFS::Path::Normalized model = getModel(ptr);
                        if (!model.empty())
                        {
                            const auto rotation = makeNodeRotation(ptr, RotationOrder::direct);
                            ptr.getClass().insertObjectPhysics(ptr, model, rotation, *mPhysics);
                        }
                    }
                }
                return true;
            });
        }
    }

    void Scene::testExteriorCells()
    {
        if (!mRendering)
            return;

        Resource::ResourceSystem& resourceSystem = *MWBase::Environment::get().getResourceSystem();
        Resource::SceneManager* const sceneManager = resourceSystem.getSceneManager();
        if (!sceneManager)
            return;
        // Note: temporary disable ICO to decrease memory usage
        osgUtil::IncrementalCompileOperation* const incrementalCompileOperation
            = sceneManager->getIncrementalCompileOperation();
        sceneManager->setIncrementalCompileOperation(nullptr);

        resourceSystem.setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getExtSize());

        MWWorld::Store<ESM::Cell>::iterator it = cells.extBegin();
        int i = 1;
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.extEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingExteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getExtSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getExterior(
                ESM::ExteriorCellLocation(it->mData.mX, it->mData.mY, ESM::Cell::sDefaultWorldspaceId));
            const osg::Vec3f position
                = osg::Vec3f(it->mData.mX + 0.5f, it->mData.mY + 0.5f, 0) * Constants::CellSizeInUnits;
            const osg::Vec2i cellPosition(it->mData.mX, it->mData.mY);

            const DetourNavigator::CellGridBounds cellGridBounds{
                .mCenter = osg::Vec2i(it->mData.mX, it->mData.mY),
                .mHalfSize = Constants::CellGridRadius,
            };

            mNavigator.updateBounds(
                ESM::Cell::sDefaultWorldspaceId, cellGridBounds, position, navigatorUpdateGuard.get());

            loadCell(cell, nullptr, false, navigatorUpdateGuard.get());

            mNavigator.update(position, navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                if (it->isExterior() && it->mData.mX == (*iter)->getCell()->getGridX()
                    && it->mData.mY == (*iter)->getCell()->getGridY())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            resourceSystem.updateCache(mFrameLifecycle.referenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        sceneManager->setIncrementalCompileOperation(incrementalCompileOperation);
        resourceSystem.setExpiryDelay(Settings::cells().mCacheExpiryDelay);
    }

    void Scene::testInteriorCells()
    {
        if (!mRendering)
            return;

        Resource::ResourceSystem& resourceSystem = *MWBase::Environment::get().getResourceSystem();
        Resource::SceneManager* const sceneManager = resourceSystem.getSceneManager();
        if (!sceneManager)
            return;
        // Note: temporary disable ICO to decrease memory usage
        osgUtil::IncrementalCompileOperation* const incrementalCompileOperation
            = sceneManager->getIncrementalCompileOperation();
        sceneManager->setIncrementalCompileOperation(nullptr);

        resourceSystem.setExpiryDelay(1.f);

        const MWWorld::Store<ESM::Cell>& cells = mWorld.getStore().get<ESM::Cell>();

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);
        loadingListener->setProgressRange(cells.getIntSize());

        int i = 1;
        MWWorld::Store<ESM::Cell>::iterator it = cells.intBegin();
        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();
        for (; it != cells.intEnd(); ++it)
        {
            loadingListener->setLabel("#{OMWEngine:TestingInteriorCells} (" + std::to_string(i) + "/"
                + std::to_string(cells.getIntSize()) + ")...");

            CellStore& cell = mWorld.getWorldModel().getInterior(it->mName);
            ESM::Position position;
            mWorld.findInteriorPosition(it->mName, position);
            mNavigator.updateBounds(
                cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());
            loadCell(cell, nullptr, false, navigatorUpdateGuard.get());

            mNavigator.update(position.asVec3(), navigatorUpdateGuard.get());
            navigatorUpdateGuard.reset();
            mNavigator.wait(DetourNavigator::WaitConditionType::requiredTilesPresent, nullptr);
            navigatorUpdateGuard = mNavigator.makeUpdateGuard();

            auto iter = mActiveCells.begin();
            while (iter != mActiveCells.end())
            {
                assert(!(*iter)->getCell()->isExterior());

                if (it->mName == (*iter)->getCell()->getNameId())
                {
                    unloadCell(*iter, navigatorUpdateGuard.get());
                    break;
                }

                ++iter;
            }

            resourceSystem.updateCache(mFrameLifecycle.referenceTime());

            loadingListener->increaseProgress(1);
            i++;
        }

        sceneManager->setIncrementalCompileOperation(incrementalCompileOperation);
        resourceSystem.setExpiryDelay(Settings::cells().mCacheExpiryDelay);
    }

    void Scene::changePlayerCell(CellStore& cell, const ESM::Position& pos, bool adjustPlayerPos)
    {
        mHalfGridSize = cell.getCell()->isEsm4() ? Constants::ESM4CellGridRadius : Constants::CellGridRadius;
        mCurrentCell = &cell;

        if (mNeutralWorldScene)
        {
            const std::string worldspace = cell.getCell()->getWorldSpace().serializeText();
            if (mNeutralWorldScene->activeWorldspace() != worldspace)
                mNeutralWorldScene->clearEffects();
            mNeutralWorldScene->setActiveWorldspace(worldspace);
        }
        mNeutralTerrainRegionsDirty = true;
        if (mRendering)
            mRendering->enableTerrain(cell.isExterior(), cell.getCell()->getWorldSpace());

        MWWorld::Ptr old = mWorld.getPlayerPtr();
        mWorld.getPlayer().setCell(&cell);

        MWWorld::Ptr player = mWorld.getPlayerPtr();
        if (mRendering)
            mRendering->updatePlayerPtr(player);

        // The player is loaded before the scene and by default it is grounded, with the scene fully loaded,
        // we validate and correct this. Only run once, during initial cell load.
        if (old.mCell == &cell)
            mPhysics->traceDown(player, player.getRefData().getPosition().asVec3(), 10.f);

        if (adjustPlayerPos)
        {
            mWorld.moveObject(player, pos.asVec3());
            mWorld.rotateObject(player, pos.asRotationVec3());

            player.getClass().adjustPosition(player, true);
        }

        MWBase::Environment::get().getMechanicsManager()->updateCell(old, player);
        if (mRendering)
            MWBase::Environment::get().getWindowManager()->watchActor(player);

        mPhysics->updatePtr(old, player);

        mWorld.adjustSky();

        const auto& playerPosition = player.getRefData().getPosition();
        mLastPlayerPos = { playerPosition.pos[0], playerPosition.pos[1], playerPosition.pos[2] };
    }

    Scene::Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
        Render::SceneSynchronizer sceneSynchronizer, Render::MeshResolver meshResolver,
        Render::TextureResolver textureResolver, Render::PoseResolver poseResolver, const VFS::Manager* vfs,
        MWRender::RenderingManager* rendering, MWRender::ObjectPaging* objectPaging,
        Terrain::RenderStorage& terrainStorage, std::unique_ptr<CellPreloader> preloader,
        MWPhysics::PhysicsSystem* physics,
        DetourNavigator::Navigator& navigator)
        : mCurrentCell(nullptr)
        , mCellChanged(false)
        , mWorld(world)
        , mFrameLifecycle(frameLifecycle)
        , mSceneSynchronizer(std::move(sceneSynchronizer))
        , mMeshResolver(std::move(meshResolver))
        , mTextureResolver(std::move(textureResolver))
        , mPoseResolver(std::move(poseResolver))
        , mVfs(vfs)
        , mPhysics(physics)
        , mRendering(rendering)
        , mObjectPaging(objectPaging)
        , mTerrainStorage(terrainStorage)
        , mNavigator(navigator)
        , mPreloader(std::move(preloader))
        , mLowestPoint(std::numeric_limits<float>::max())
    {
    }

    Scene::Scene(MWWorld::World& world, Render::FrameLifecycle& frameLifecycle,
        Render::SceneSynchronizer sceneSynchronizer, Render::MeshResolver meshResolver,
        Render::TextureResolver textureResolver, Render::PoseResolver poseResolver, const VFS::Manager* vfs,
        Terrain::RenderStorage& terrainStorage,
        MWPhysics::PhysicsSystem* physics, DetourNavigator::Navigator& navigator)
        : Scene(world, frameLifecycle, std::move(sceneSynchronizer), std::move(meshResolver),
            std::move(textureResolver), std::move(poseResolver), vfs, nullptr, nullptr, terrainStorage, nullptr,
            physics, navigator)
    {
        mNeutralWorldScene = std::make_unique<Render::WorldScene>();
    }

    Scene::~Scene() = default;

    bool Scene::hasCellChanged() const
    {
        return mCellChanged;
    }

    const Scene::CellStoreCollection& Scene::getActiveCells() const
    {
        return mActiveCells;
    }

    void Scene::changeToInteriorCell(
        std::string_view cellName, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {
        CellStore& cell = mWorld.getWorldModel().getInterior(cellName);
        bool useFading = mRendering && (mCurrentCell != nullptr);
        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);

        Loading::Listener* loadingListener
            = mRendering ? MWBase::Environment::get().getWindowManager()->getLoadingScreen() : nullptr;
        if (loadingListener)
            loadingListener->setLabel("#{OMWEngine:LoadingInterior}");
        Loading::ScopedLoad load(loadingListener);

        if (mCurrentCell == &cell)
        {
            mWorld.moveObject(mWorld.getPlayerPtr(), position.asVec3());
            mWorld.rotateObject(mWorld.getPlayerPtr(), position.asRotationVec3());

            if (adjustPlayerPos)
                mWorld.getPlayerPtr().getClass().adjustPosition(mWorld.getPlayerPtr(), true);
            if (mRendering)
                MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);
            return;
        }

        Log(Debug::Info) << "Changing to interior";

        auto navigatorUpdateGuard = mNavigator.makeUpdateGuard();

        // unload
        for (auto iter = mActiveCells.begin(); iter != mActiveCells.end();)
        {
            auto* cellToUnload = *iter++;
            unloadCell(cellToUnload, navigatorUpdateGuard.get());
        }
        assert(mActiveCells.empty());

        if (loadingListener)
            loadingListener->setProgressRange(cell.count());

        mNavigator.updateBounds(
            cell.getCell()->getWorldSpace(), std::nullopt, position.asVec3(), navigatorUpdateGuard.get());

        // Load cell.
        loadCell(cell, loadingListener, changeEvent, navigatorUpdateGuard.get());

        navigatorUpdateGuard.reset();

        changePlayerCell(cell, position, adjustPlayerPos);

        // adjust fog
        if (mRendering)
            mRendering->configureFog(*mCurrentCell->getCell());

        // Sky system
        mWorld.adjustSky();

        if (changeEvent)
            mCellChanged = true;

        mCellLoaded = true;

        if (useFading)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        if (mRendering)
            MWBase::Environment::get().getWindowManager()->changeCell(mCurrentCell);

        if (mRendering)
            MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(cell.getCell()->isQuasiExterior());
    }

    void Scene::changeToExteriorCell(
        const ESM::RefId& extCellId, const ESM::Position& position, bool adjustPlayerPos, bool changeEvent)
    {

        if (mRendering && changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenOut(0.5);
        CellStore& current = mWorld.getWorldModel().getCell(extCellId);

        const osg::Vec2i cellIndex(current.getCell()->getGridX(), current.getCell()->getGridY());

        changeCellGrid({ position.pos[0], position.pos[1], position.pos[2] },
            ESM::ExteriorCellLocation(cellIndex.x(), cellIndex.y(), current.getCell()->getWorldSpace()), changeEvent);

        changePlayerCell(current, position, adjustPlayerPos);

        if (mRendering && changeEvent)
            MWBase::Environment::get().getWindowManager()->fadeScreenIn(0.5);

        if (mRendering)
            MWBase::Environment::get().getWorld()->getPostProcessor()->setExteriorFlag(true);
    }

    CellStore* Scene::getCurrentCell()
    {
        return mCurrentCell;
    }

    void Scene::updateNeutralTerrainRegions()
    {
        if (!mNeutralWorldScene)
            return;

        mNeutralTerrainRegionsDirty = false;
        mNeutralWorldScene->setTerrainRegions({});

        if (mActiveCells.empty() || mNeutralWorldScene->activeWorldspace().empty())
            return;

        bool foundExteriorCell = false;
        ESM::RefId worldspace;
        std::set<std::pair<int, int>> activeExteriorCells;
        for (const CellStore* cell : mActiveCells)
        {
            if (!cell->isExterior())
                continue;

            const ESM::RefId cellWorldspace = cell->getCell()->getWorldSpace();
            if (cellWorldspace.serializeText() != mNeutralWorldScene->activeWorldspace())
                continue;

            const int cellX = cell->getCell()->getGridX();
            const int cellY = cell->getCell()->getGridY();
            if (!foundExteriorCell)
            {
                foundExteriorCell = true;
                worldspace = cellWorldspace;
            }
            activeExteriorCells.emplace(cellX, cellY);
        }

        if (!foundExteriorCell)
            return;

        std::vector<Render::TerrainRegion> regions
            = mTerrainStorage.getRenderRegionTiles(activeExteriorCells, worldspace);
        if (regions.empty()
            || !std::all_of(regions.begin(), regions.end(), [](const Render::TerrainRegion& region) {
                   return region.valid();
               }))
            return;

        mNeutralWorldScene->setTerrainRegions(std::move(regions));
    }

    Render::SceneSubmission Scene::getNeutralScene()
    {
        if (!mNeutralWorldScene)
            throw std::logic_error("Neutral scene requested from the OSG world path");

        if (mNeutralTerrainRegionsDirty)
            updateNeutralTerrainRegions();

        if (mSceneSynchronizer)
            mSceneSynchronizer(mNeutralWorldScene->sceneData());

        const auto resolveMeshes = [this](std::string_view model) -> const std::vector<Render::MeshInstance>& {
            const std::string key(model);
            const auto found = mNeutralMeshCache.find(key);
            std::shared_ptr<const std::vector<Render::MeshInstance>> meshes
                = found == mNeutralMeshCache.end() ? nullptr : found->second.lock();
            if (!meshes && mMeshResolver)
                meshes = mMeshResolver(model);
            if (!meshes)
                meshes = std::make_shared<const std::vector<Render::MeshInstance>>();
            mNeutralMeshCache[key] = meshes;
            return *meshes;
        };

        Render::SceneSubmission result = Render::collectSceneSubmission(
            *mNeutralWorldScene, mNeutralWorldScene->sceneData(), mNeutralWorldScene->activeWorldspace(), resolveMeshes,
            true, false, false);

        for (Render::EffectMeshSubmission& effect : result.effects)
        {
            const Render::SkinningData* skinning = Render::findPrimarySkinning(effect);
            bool posed = false;
            if (mPoseResolver && skinning != nullptr)
            {
                const std::vector<Render::Mat4> pose = mPoseResolver(effect.object.model,
                    effect.object.animationSources, effect.object.animationGroup, effect.object.animationTime,
                    effect.object.animationLooping, effect.object.animationStartKey, effect.object.animationStopKey,
                    skinning->boneNames);
                if (pose.size() == skinning->inverseBindMatrices.size())
                {
                    for (Render::MeshInstance& mesh : effect.meshes)
                        if (mesh.mesh.skinning)
                        {
                            const std::vector<Render::Mat4> remapped = Render::remapBoneMatrices(
                                pose, skinning->boneNames, mesh.mesh.skinning->boneNames);
                            if (remapped.size() >= mesh.mesh.skinning->inverseBindMatrices.size())
                            {
                                mesh.mesh = Render::skinMesh(mesh.mesh, remapped);
                                posed = true;
                            }
                            else
                                Render::bakeMeshBindPose(mesh);
                        }
                }
            }
            if (!posed)
                for (Render::MeshInstance& mesh : effect.meshes)
                    Render::bakeMeshBindPose(mesh);
        }

        for (Render::DynamicMeshSubmission& dynamic : result.dynamicMeshes)
        {
            if (mPoseResolver && dynamic.boneMatrices.empty())
            {
                const Render::SkinningData* skinning = Render::findPrimarySkinning(dynamic);
                if (skinning != nullptr)
                {
                    const std::vector<Render::Mat4> pose = mPoseResolver(dynamic.object.model,
                        dynamic.object.animationSources, dynamic.object.animationGroup, dynamic.object.animationTime,
                        dynamic.object.animationLooping, dynamic.object.animationStartKey,
                        dynamic.object.animationStopKey, skinning->boneNames);
                    if (pose.size() == skinning->inverseBindMatrices.size())
                    {
                        dynamic.boneMatrices = pose;
                        dynamic.boneNames = skinning->boneNames;
                    }
                }
            }

            Render::applyBindPose(dynamic);
            if (!dynamic.boneMatrices.empty())
            {
                const Render::SkinningData* skinning = Render::findPrimarySkinning(dynamic);
                if (skinning != nullptr)
                {
                    for (const Render::WorldObject::Attachment& attachment : dynamic.object.attachments)
                    {
                        if (!attachment.visible || attachment.model.empty() || attachment.bone.empty())
                            continue;
                        const auto bone = std::find(skinning->boneNames.begin(), skinning->boneNames.end(), attachment.bone);
                        if (bone == skinning->boneNames.end())
                            continue;
                        const std::size_t boneIndex = static_cast<std::size_t>(bone - skinning->boneNames.begin());
                        if (boneIndex >= dynamic.boneMatrices.size())
                            continue;
                        const std::vector<Render::MeshInstance>& meshes = resolveMeshes(attachment.model);
                        if (!std::any_of(meshes.begin(), meshes.end(), Render::hasRenderableGeometry))
                        {
                            result.unresolvedModels.push_back(attachment.model);
                            continue;
                        }
                        for (const Render::MeshInstance& mesh : meshes)
                        {
                            Render::MeshInstance attached = mesh;
                            Render::bakeMeshBindPose(attached);
                            attached.transform = Render::multiply(dynamic.boneMatrices[boneIndex], mesh.transform);
                            dynamic.meshes.push_back(Render::transformMeshInstance(dynamic.object, attached));
                        }
                    }
                }
            }

        }

        if (Settings::shaders().mAutoUseObjectSpecularMaps
            && !Settings::shaders().mSpecularMapPattern.get().empty())
        {
            const std::string& pattern = Settings::shaders().mSpecularMapPattern;
            const auto addSpecularMap = [&](Render::MeshInstance& instance) {
                Render::MeshMaterial& material = instance.mesh.material;
                if (material.albedoTexture.empty() || !material.specularTexture.empty())
                    return;

                const std::string specularPath
                    = Render::makeSpecularTexturePath(material.albedoTexture, pattern);
                if (specularPath.empty())
                    return;
                const VFS::Path::Normalized specularMap(specularPath);
                if (mVfs == nullptr || !mVfs->exists(specularMap))
                    return;

                material.specularTexture = specularMap.value();
                material.specularWrapU = material.albedoWrapU;
                material.specularWrapV = material.albedoWrapV;
            };
            for (Render::MeshInstance& instance : result.meshes)
                addSpecularMap(instance);
            for (Render::DynamicMeshSubmission& dynamic : result.dynamicMeshes)
                for (Render::MeshInstance& instance : dynamic.meshes)
                    addSpecularMap(instance);
        }
        result.textureResolver = mTextureResolver;
        return result;
    }

    void Scene::markCellAsUnchanged()
    {
        mCellChanged = false;
    }

    void Scene::insertCell(
        CellStore& cell, Loading::Listener* loadingListener, const DetourNavigator::UpdateGuard* navigatorUpdateGuard)
    {
        const bool isInterior = !cell.isExterior();
        InsertVisitor insertVisitor(cell, loadingListener);
        cell.forEach(insertVisitor);
        insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
            addObject(ptr, mWorld, mObjectPaging, *mPhysics, mRendering, mNeutralWorldScene.get());
        });
        insertVisitor.insert([&](const MWWorld::Ptr& ptr) {
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator, navigatorUpdateGuard);
        });
    }

    void Scene::addObjectToScene(const Ptr& ptr)
    {
        const bool isInterior = mCurrentCell && !mCurrentCell->isExterior();
        try
        {
            addObject(ptr, mWorld, mObjectPaging, *mPhysics, mRendering, mNeutralWorldScene.get());
            addObject(ptr, mWorld, *mPhysics, mLowestPoint, isInterior, mNavigator);
            mWorld.scaleObject(ptr, ptr.getCellRef().getScale());
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "failed to render '" << ptr.getCellRef().getRefId() << "': " << e.what();
        }
    }

    void Scene::removeObjectFromScene(const Ptr& ptr, bool keepActive)
    {
        MWBase::Environment::get().getMechanicsManager()->remove(ptr, keepActive);
        // You'd expect the sounds attached to the object to be stopped here
        // because the object is nowhere to be heard, but in Morrowind, they're not.
        // They're still stopped when the cell is unloaded
        // or if the player moves away far from the object's position.
        // Todd Howard, Who art in Bethesda, hallowed be Thy name.
        MWBase::Environment::get().getLuaManager()->objectRemovedFromScene(ptr);
        if (const auto object = mPhysics->getObject(ptr))
        {
            if (object->getShapeInstance()->mVisualCollisionType == Resource::VisualCollisionType::None)
                mNavigator.removeObject(DetourNavigator::ObjectId(object), nullptr);
        }
        else if (mPhysics->getActor(ptr))
        {
            mNavigator.removeAgent(mWorld.getPathfindingAgentBounds(ptr));
        }
        mPhysics->remove(ptr);
        if (!ptr.isEmpty())
            if (mNeutralWorldScene)
                mNeutralWorldScene->removeObject(static_cast<const void*>(ptr.mRef));
        if (mRendering)
            mRendering->removeObject(ptr);
        if (ptr.getClass().isActor())
        {
            if (mRendering)
                mRendering->removeWaterRippleEmitter(ptr);
        }
        ptr.getRefData().setBaseNode(nullptr);
    }

    bool Scene::isCellActive(const CellStore& cell)
    {
        return mActiveCells.contains(&cell);
    }

    void Scene::preload(const std::string& mesh, bool useAnim)
    {
        if (!mPreloader)
            return;
        mPreloader->preloadMesh(mesh, useAnim, mFrameLifecycle.referenceTime());
    }

    void Scene::preloadCells(float dt)
    {
        if (!mPreloader || dt <= 1e-06)
            return;
        const auto& cellSettings = Settings::cells();
        std::vector<PositionCellGrid> exteriorPositions;

        const MWWorld::ConstPtr player = mWorld.getPlayerPtr();
        const auto& position = player.getRefData().getPosition();
        Render::Vec3 playerPos{ position.pos[0], position.pos[1], position.pos[2] };
        Render::Vec3 moved{ playerPos.x - mLastPlayerPos.x, playerPos.y - mLastPlayerPos.y,
            playerPos.z - mLastPlayerPos.z };
        Render::Vec3 predictedPos{ playerPos.x + moved.x / dt * cellSettings.mPredictionTime,
            playerPos.y + moved.y / dt * cellSettings.mPredictionTime,
            playerPos.z + moved.z / dt * cellSettings.mPredictionTime };

        if (mCurrentCell->isExterior())
            exteriorPositions.push_back(
                makeTerrainPreloadPosition(
                    predictedPos, gridCenterToBounds(getNewGridCenter(predictedPos, &mCurrentGridCenter))));

        mLastPlayerPos = playerPos;

        if (cellSettings.mPreloadEnabled)
        {
            if (cellSettings.mPreloadDoors)
                preloadTeleportDoorDestinations(playerPos, predictedPos);
            if (cellSettings.mPreloadExteriorGrid)
                preloadExteriorGrid(playerPos, predictedPos);
            if (cellSettings.mPreloadFastTravel)
                preloadFastTravelDestinations(playerPos, exteriorPositions);
        }

        mPreloader->setTerrainPreloadPositions(exteriorPositions);
    }

    void Scene::preloadTeleportDoorDestinations(const Render::Vec3& playerPos, const Render::Vec3& predictedPos)
    {
        const float preloadDistance = Settings::cells().mPreloadDistance;
        std::vector<MWWorld::ConstPtr> teleportDoors;
        for (const MWWorld::CellStore* cellStore : mActiveCells)
        {
            typedef MWWorld::CellRefList<ESM::Door>::List DoorList;
            const DoorList& doors = cellStore->getReadOnlyDoors().mList;
            for (auto& door : doors)
            {
                if (!door.mRef.getTeleport())
                {
                    continue;
                }
                teleportDoors.emplace_back(&door, cellStore);
            }
        }

        for (const MWWorld::ConstPtr& door : teleportDoors)
        {
            const auto& doorPosition = door.getRefData().getPosition();
            const Render::Vec3 doorPos{ doorPosition.pos[0], doorPosition.pos[1], doorPosition.pos[2] };
            const auto squaredDistance = [](const Render::Vec3& lhs, const Render::Vec3& rhs) {
                const float x = lhs.x - rhs.x;
                const float y = lhs.y - rhs.y;
                const float z = lhs.z - rhs.z;
                return x * x + y * y + z * z;
            };
            float sqrDistToPlayer = squaredDistance(playerPos, doorPos);
            sqrDistToPlayer = std::min(sqrDistToPlayer, squaredDistance(predictedPos, doorPos));

            if (sqrDistToPlayer < preloadDistance * preloadDistance)
            {
                try
                {
                    preloadCellWithSurroundings(mWorld.getWorldModel().getCell(door.getCellRef().getDestCell()));
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "Failed to schedule preload for door " << door.toString() << ": "
                                        << e.what();
                }
            }
        }
    }

    void Scene::preloadExteriorGrid(const Render::Vec3& playerPos, const Render::Vec3& predictedPos)
    {
        if (!mWorld.isCellExterior())
            return;
        const float preloadDistance = Settings::cells().mPreloadDistance;
        constexpr float cellLoadingThreshold = 1024.f;

        int halfGridSizePlusOne = mHalfGridSize + 1;

        int cellX, cellY;
        cellX = mCurrentGridCenter[0];
        cellY = mCurrentGridCenter[1];
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();

        int cellSize = ESM::getCellSize(extWorldspace);

        for (int dx = -halfGridSizePlusOne; dx <= halfGridSizePlusOne; ++dx)
        {
            for (int dy = -halfGridSizePlusOne; dy <= halfGridSizePlusOne; ++dy)
            {
                if (dy != halfGridSizePlusOne && dy != -halfGridSizePlusOne && dx != halfGridSizePlusOne
                    && dx != -halfGridSizePlusOne)
                    continue; // only care about the outer (not yet loaded) part of the grid
                ESM::ExteriorCellLocation cellIndex(cellX + dx, cellY + dy, extWorldspace);
                const osg::Vec2f thisCellCenter = ESM::indexToPosition(cellIndex, true);

                float dist = std::max(
                    std::abs(thisCellCenter.x() - playerPos.x), std::abs(thisCellCenter.y() - playerPos.y));
                dist = std::min(dist,
                    std::max(std::abs(thisCellCenter.x() - predictedPos.x),
                        std::abs(thisCellCenter.y() - predictedPos.y)));
                const float loadDist = cellSize / 2 + cellSize - cellLoadingThreshold + preloadDistance;

                if (dist < loadDist)
                    preloadCell(mWorld.getWorldModel().getExterior(cellIndex));
            }
        }
    }

    void Scene::preloadCellWithSurroundings(CellStore& cell)
    {
        if (!mPreloader)
            return;

        if (!cell.isExterior())
        {
            mPreloader->preload(cell, mFrameLifecycle.referenceTime());
            return;
        }

        const int cellX = cell.getCell()->getGridX();
        const int cellY = cell.getCell()->getGridY();

        std::vector<std::pair<int, int>> cells;
        const std::size_t gridSize = static_cast<std::size_t>(2 * mHalfGridSize + 1);
        cells.reserve(gridSize * gridSize);

        iterateOverCellsAround(cellX, cellY, mHalfGridSize, [&](int x, int y) { cells.emplace_back(x, y); });

        sortCellsToLoad(cellX, cellY, cells);

        const std::size_t leftCapacity = mPreloader->getMaxCacheSize() - mPreloader->getCacheSize();
        if (cells.size() > leftCapacity)
        {
            [[maybe_unused]] static const bool logged = [&] {
                Log(Debug::Warning) << "Not enough cell preloader cache capacity to preload exterior cells, consider "
                                       "increasing \"preload cell cache max\" up to "
                                    << (mPreloader->getCacheSize() + cells.size());
                return true;
            }();
            cells.resize(leftCapacity);
        }

        const ESM::RefId worldspace = cell.getCell()->getWorldSpace();
        for (const auto& [x, y] : cells)
            mPreloader->preload(mWorld.getWorldModel().getExterior(ESM::ExteriorCellLocation(x, y, worldspace)),
                mFrameLifecycle.referenceTime());
    }

    void Scene::preloadCell(CellStore& cell)
    {
        if (mPreloader)
            mPreloader->preload(cell, mFrameLifecycle.referenceTime());
    }

    void Scene::preloadTerrain(const Render::Vec3& pos, ESM::RefId worldspace, bool sync)
    {
        if (!mPreloader)
            return;

        if (!mPreloader->terrainWorldspaceMatches(worldspace))
            throw std::runtime_error("preloadTerrain can only work with the current exterior worldspace");

        ESM::ExteriorCellLocation cellPos = ESM::positionToExteriorCellLocation(pos.x, pos.y, worldspace);
        const PositionCellGrid position = makeTerrainPreloadPosition(pos, gridCenterToBounds({ cellPos.mX, cellPos.mY }));
        mPreloader->abortTerrainPreloadExcept(&position);
        mPreloader->setTerrainPreloadPositions(std::span(&position, 1));
        if (!sync)
            return;

        Loading::Listener* loadingListener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
        Loading::ScopedLoad load(loadingListener);

        loadingListener->setLabel("#{OMWEngine:InitializingData}");

        mPreloader->syncTerrainLoad(*loadingListener);
    }

    void Scene::reloadTerrain()
    {
        mNeutralTerrainRegionsDirty = true;
        if (mPreloader)
            mPreloader->setTerrainPreloadPositions({});
    }

    struct ListFastTravelDestinationsVisitor
    {
        ListFastTravelDestinationsVisitor(float preloadDist, const Render::Vec3& playerPos)
            : mPreloadDist(preloadDist)
            , mPlayerPos(playerPos)
        {
        }

        bool operator()(const MWWorld::Ptr& ptr)
        {
            const auto& position = ptr.getRefData().getPosition();
            const Render::Vec3 ptrPosition{ position.pos[0], position.pos[1], position.pos[2] };
            const float x = ptrPosition.x - mPlayerPos.x;
            const float y = ptrPosition.y - mPlayerPos.y;
            const float z = ptrPosition.z - mPlayerPos.z;
            if (x * x + y * y + z * z > mPreloadDist * mPreloadDist)
                return true;

            if (ptr.getClass().isNpc())
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::NPC>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            else
            {
                const std::vector<ESM::Transport::Dest>& transport = ptr.get<ESM::Creature>()->mBase->mTransport.mList;
                mList.insert(mList.begin(), transport.begin(), transport.end());
            }
            return true;
        }
        float mPreloadDist;
        Render::Vec3 mPlayerPos;
        std::vector<ESM::Transport::Dest> mList;
    };

    void Scene::preloadFastTravelDestinations(
        const Render::Vec3& playerPos, std::vector<PositionCellGrid>& exteriorPositions)
    {
        ListFastTravelDestinationsVisitor listVisitor(Settings::cells().mPreloadDistance, playerPos);
        ESM::RefId extWorldspace = mWorld.getCurrentWorldspace();
        for (MWWorld::CellStore* cellStore : mActiveCells)
        {
            cellStore->forEachType<ESM::NPC>(listVisitor);
            cellStore->forEachType<ESM::Creature>(listVisitor);
        }

        for (ESM::Transport::Dest& dest : listVisitor.mList)
        {
            if (!dest.mCellName.empty())
                preloadCell(mWorld.getWorldModel().getInterior(dest.mCellName));
            else
            {
                osg::Vec3f pos = dest.mPos.asVec3();
                const ESM::ExteriorCellLocation cellIndex
                    = ESM::positionToExteriorCellLocation(pos.x(), pos.y(), extWorldspace);
                preloadCellWithSurroundings(mWorld.getWorldModel().getExterior(cellIndex));
                exteriorPositions.push_back(makeTerrainPreloadPosition({ pos.x(), pos.y(), pos.z() },
                    gridCenterToBounds(getNewGridCenter({ pos.x(), pos.y(), pos.z() }))));
            }
        }
    }

    void Scene::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        if (mPreloader)
            mPreloader->reportStats(frameNumber, stats);
    }
}
