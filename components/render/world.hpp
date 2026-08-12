#ifndef OPENMW_COMPONENTS_RENDER_WORLD_H
#define OPENMW_COMPONENTS_RENDER_WORLD_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "math.hpp"
#include "terrain.hpp"

namespace Render
{
    struct ObjectTransform
    {
        Vec3 position{};
        Quat rotation{ 0.f, 0.f, 0.f, 1.f };
        Vec3 scale{ 1.f, 1.f, 1.f };

        bool valid() const
        {
            return Render::valid(position) && Render::valid(rotation) && Render::valid(scale);
        }
    };

    // Renderer-neutral state for one loaded world reference. The id is owned by
    // the scene bridge and is intentionally opaque to backends.
    struct WorldObject
    {
        uint64_t id = 0;
        std::string model;
        ObjectTransform transform;
        bool visible = true;
        bool dynamic = false;
        // Optional frame pose supplied by the animation owner. The matrices
        // use the skinning order of the resolved mesh and contain no backend
        // or scene-graph types.
        std::vector<Mat4> boneMatrices;
    };

    // A cell snapshot is updated by the world lifecycle, not by a renderer.
    struct CellScene
    {
        const void* key = nullptr;
        bool exterior = false;
        int gridX = 0;
        int gridY = 0;
        std::string worldspace;
        std::string name;
        std::vector<WorldObject> objects;
        std::vector<TerrainTile> terrainTiles;

        WorldObject* findObject(uint64_t id)
        {
            const auto found = std::find_if(objects.begin(), objects.end(), [id](const WorldObject& object) {
                return object.id == id;
            });
            return found == objects.end() ? nullptr : &*found;
        }

        const WorldObject* findObject(uint64_t id) const
        {
            const auto found = std::find_if(objects.begin(), objects.end(), [id](const WorldObject& object) {
                return object.id == id;
            });
            return found == objects.end() ? nullptr : &*found;
        }

        bool eraseObject(uint64_t id)
        {
            const auto oldSize = objects.size();
            std::erase_if(objects, [id](const WorldObject& object) { return object.id == id; });
            return objects.size() != oldSize;
        }
    };

    // Renderer-neutral ownership for loaded cells and their object identity.
    // The keys are opaque engine-owned handles; no renderer or game type leaks
    // into this component.
    class WorldScene
    {
        struct ObjectLocation
        {
            const void* cell;
            uint64_t id;
        };

        std::unordered_map<const void*, CellScene> mCells;
        std::unordered_map<const void*, ObjectLocation> mObjects;
        std::vector<TerrainRegion> mTerrainRegions;
        std::string mActiveWorldspace;
        SceneData mSceneData{};
        uint64_t mNextObjectId = 1;

        CellScene& ensureCell(const void* cell, bool exterior, int gridX, int gridY, std::string_view name,
            std::string_view worldspace)
        {
            auto iter = mCells.try_emplace(cell).first;

            CellScene& scene = iter->second;
            scene.key = cell;
            scene.exterior = exterior;
            scene.gridX = gridX;
            scene.gridY = gridY;
            scene.worldspace = worldspace;
            scene.name = name;
            return scene;
        }

        void rekeyObject(const void* oldKey, const void* newKey, const ObjectLocation& location)
        {
            if (oldKey == newKey)
                return;
            mObjects.erase(oldKey);
            mObjects.emplace(newKey, location);
        }

        template <class Update>
        bool updateObjectTransform(const void* objectKey, Update&& update)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr)
                return false;
            update(object->transform);
            return true;
        }

    public:
        void setActiveWorldspace(std::string_view worldspace) { mActiveWorldspace = worldspace; }

        std::string_view activeWorldspace() const { return mActiveWorldspace; }

        SceneData& sceneData() { return mSceneData; }

        void recordCell(const void* cellKey, bool exterior, int gridX, int gridY, std::string_view name,
            std::string_view worldspace = {})
        {
            if (cellKey == nullptr)
                return;
            ensureCell(cellKey, exterior, gridX, gridY, name, worldspace);
        }

        void setTerrainTiles(const void* cellKey, std::vector<TerrainTile> tiles)
        {
            if (cellKey == nullptr)
                return;
            const auto found = mCells.find(cellKey);
            if (found != mCells.end())
                found->second.terrainTiles = std::move(tiles);
        }

        void setTerrainRegions(std::vector<TerrainRegion> regions) { mTerrainRegions = std::move(regions); }

        const std::vector<TerrainRegion>& terrainRegions() const { return mTerrainRegions; }

        // Add renderer-owned static instances whose lifetime is tied to a cell
        // but which do not have an engine reference identity, such as groundcover.
        void recordStaticObject(const void* cellKey, bool exterior, int gridX, int gridY, std::string_view cellName,
            std::string_view model, const ObjectTransform& transform, bool visible,
            std::string_view worldspace = {})
        {
            if (cellKey == nullptr || model.empty())
                return;

            WorldObject object;
            object.id = mNextObjectId++;
            if (object.id == 0)
                object.id = mNextObjectId++;
            object.model = model;
            object.transform = transform;
            object.visible = visible;
            object.dynamic = false;
            ensureCell(cellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(object));
        }

        void recordObject(const void* objectKey, const void* cellKey, bool exterior, int gridX, int gridY,
            std::string_view cellName, std::string_view model, const ObjectTransform& transform, bool visible,
            std::string_view worldspace = {}, bool dynamic = false)
        {
            if (objectKey == nullptr || cellKey == nullptr || model.empty())
            {
                removeObject(objectKey);
                return;
            }

            const auto found = mObjects.find(objectKey);
            if (found != mObjects.end())
            {
                const ObjectLocation location = found->second;
                if (location.cell != cellKey)
                {
                    if (updateObjectCell(objectKey, objectKey, cellKey, exterior, gridX, gridY, cellName, worldspace))
                        return recordObject(
                            objectKey, cellKey, exterior, gridX, gridY, cellName, model, transform, visible, worldspace,
                            dynamic);
                    mObjects.erase(found);
                }
                else if (CellScene* scene = findCell(location.cell))
                {
                    if (WorldObject* object = scene->findObject(location.id))
                    {
                        object->model = model;
                        object->transform = transform;
                        object->visible = visible;
                        object->dynamic = dynamic;
                        if (!dynamic)
                            object->boneMatrices.clear();
                        return;
                    }
                }
                mObjects.erase(found);
            }

            WorldObject object;
            object.id = mNextObjectId++;
            if (object.id == 0)
                object.id = mNextObjectId++;
            const uint64_t id = object.id;
            object.model = model;
            object.transform = transform;
            object.visible = visible;
            object.dynamic = dynamic;
            ensureCell(cellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(object));
            mObjects.emplace(objectKey, ObjectLocation{ cellKey, id });
        }

        bool updateObjectPosition(const void* objectKey, const Vec3& position)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.position = position; });
        }

        bool updateObjectRotation(const void* objectKey, const Quat& rotation)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.rotation = rotation; });
        }

        bool updateObjectScale(const void* objectKey, const Vec3& scale)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.scale = scale; });
        }

        bool updateObjectPose(const void* objectKey, std::vector<Mat4> boneMatrices)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;
            object->boneMatrices = std::move(boneMatrices);
            return true;
        }

        template <class ResolvePose>
        void updateDynamicPoses(ResolvePose&& resolvePose)
        {
            for (const auto& [objectKey, location] : mObjects)
            {
                const auto scene = mCells.find(location.cell);
                if (scene == mCells.end())
                    continue;
                WorldObject* object = scene->second.findObject(location.id);
                if (object == nullptr || !object->dynamic)
                    continue;
                object->boneMatrices = resolvePose(objectKey, *object);
            }
        }

        // Sorting the owned cells makes backend input deterministic without
        // maintaining a second cell-order index.
        std::vector<const CellScene*> cellsInOrder(std::string_view worldspace = {}) const
        {
            std::vector<const CellScene*> result;
            result.reserve(mCells.size());
            for (const auto& [cellKey, cell] : mCells)
            {
                if (worldspace.empty() || cell.worldspace == worldspace)
                    result.push_back(&cell);
            }
            std::stable_sort(result.begin(), result.end(), [](const CellScene* lhs, const CellScene* rhs) {
                if (lhs->exterior != rhs->exterior)
                    return lhs->exterior > rhs->exterior;
                if (lhs->worldspace != rhs->worldspace)
                    return lhs->worldspace < rhs->worldspace;
                if (lhs->gridX != rhs->gridX)
                    return lhs->gridX < rhs->gridX;
                if (lhs->gridY != rhs->gridY)
                    return lhs->gridY < rhs->gridY;
                return lhs->name < rhs->name;
            });
            return result;
        }

        CellScene* findCell(const void* cellKey)
        {
            const auto found = mCells.find(cellKey);
            return found == mCells.end() ? nullptr : &found->second;
        }

        void removeObject(const void* objectKey)
        {
            if (objectKey == nullptr)
                return;

            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return;
            const auto scene = mCells.find(found->second.cell);
            if (scene != mCells.end())
                scene->second.eraseObject(found->second.id);
            mObjects.erase(found);
        }

        bool updateObjectCell(const void* oldKey, const void* newKey, const void* newCellKey, bool exterior, int gridX,
            int gridY, std::string_view cellName = {}, std::string_view worldspace = {})
        {
            if (oldKey == nullptr || newKey == nullptr || newCellKey == nullptr)
                return false;

            const auto found = mObjects.find(oldKey);
            if (found == mObjects.end())
                return false;

            const ObjectLocation location = found->second;
            if (location.cell == newCellKey)
            {
                rekeyObject(oldKey, newKey, location);
                return true;
            }

            auto oldScene = mCells.find(location.cell);
            if (oldScene == mCells.end())
                return false;
            WorldObject* object = oldScene->second.findObject(location.id);
            if (object == nullptr)
                return false;

            WorldObject moved = std::move(*object);
            oldScene->second.eraseObject(location.id);
            ensureCell(newCellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(moved));

            mObjects.erase(found);
            mObjects.emplace(newKey, ObjectLocation{ newCellKey, location.id });
            return true;
        }

        void removeCell(const void* cellKey)
        {
            if (cellKey == nullptr)
                return;
            for (auto iter = mObjects.begin(); iter != mObjects.end();)
            {
                if (iter->second.cell == cellKey)
                    iter = mObjects.erase(iter);
                else
                    ++iter;
            }
            mCells.erase(cellKey);
        }

        // Reset renderer-neutral world ownership when the game unloads its
        // world. This is separate from removeCell so a backend can retain
        // stable cell ordering during ordinary streaming.
        void clear()
        {
            mCells.clear();
            mObjects.clear();
            mTerrainRegions.clear();
            mActiveWorldspace.clear();
            mSceneData = {};
            mNextObjectId = 1;
        }
    };

    inline Mat4 makeObjectTransformMatrix(const ObjectTransform& transform)
    {
        const float x = transform.rotation.x;
        const float y = transform.rotation.y;
        const float z = transform.rotation.z;
        const float w = transform.rotation.w;

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        Mat4 result = {};
        result.data[0] = (1.f - 2.f * (yy + zz)) * transform.scale.x;
        result.data[1] = (2.f * (xy + wz)) * transform.scale.x;
        result.data[2] = (2.f * (xz - wy)) * transform.scale.x;
        result.data[4] = (2.f * (xy - wz)) * transform.scale.y;
        result.data[5] = (1.f - 2.f * (xx + zz)) * transform.scale.y;
        result.data[6] = (2.f * (yz + wx)) * transform.scale.y;
        result.data[8] = (2.f * (xz + wy)) * transform.scale.z;
        result.data[9] = (2.f * (yz - wx)) * transform.scale.z;
        result.data[10] = (1.f - 2.f * (xx + yy)) * transform.scale.z;
        result.data[12] = transform.position.x;
        result.data[13] = transform.position.y;
        result.data[14] = transform.position.z;
        result.data[15] = 1.f;
        return result;
    }
}

#endif
