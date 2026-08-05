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

namespace Render
{
    struct ObjectTransform
    {
        Vec3 position{};
        Quat rotation{ 0.f, 0.f, 0.f, 1.f };
        Vec3 scale{ 1.f, 1.f, 1.f };
    };

    // Renderer-neutral state for one loaded world reference. The id is owned by
    // the scene bridge and is intentionally opaque to backends.
    struct WorldObject
    {
        uint64_t id = 0;
        std::string model;
        ObjectTransform transform;
        bool visible = true;
    };

    // A cell snapshot is updated by the world lifecycle, not by a renderer.
    struct CellScene
    {
        bool exterior = false;
        int gridX = 0;
        int gridY = 0;
        std::vector<WorldObject> objects;

        WorldObject* findObject(uint64_t id)
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
        uint64_t mNextObjectId = 1;

        CellScene& ensureCell(const void* cell, bool exterior, int gridX, int gridY)
        {
            CellScene& scene = mCells[cell];
            scene.exterior = exterior;
            scene.gridX = gridX;
            scene.gridY = gridY;
            return scene;
        }

        void rekeyObject(const void* oldKey, const void* newKey, const ObjectLocation& location)
        {
            if (oldKey == newKey)
                return;
            mObjects.erase(oldKey);
            mObjects.emplace(newKey, location);
        }

    public:
        void recordObject(const void* objectKey, const void* cellKey, bool exterior, int gridX, int gridY,
            std::string_view model, const ObjectTransform& transform, bool visible)
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
                    if (!updateObjectCell(objectKey, objectKey, cellKey, exterior, gridX, gridY))
                        return;
                    return recordObject(objectKey, cellKey, exterior, gridX, gridY, model, transform, visible);
                }

                if (CellScene* scene = findCell(location.cell))
                {
                    if (WorldObject* object = scene->findObject(location.id))
                    {
                        object->model = model;
                        object->transform = transform;
                        object->visible = visible;
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
            ensureCell(cellKey, exterior, gridX, gridY).objects.push_back(std::move(object));
            mObjects.emplace(objectKey, ObjectLocation{ cellKey, id });
        }

        WorldObject* findObject(const void* objectKey)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return nullptr;
            const auto scene = mCells.find(found->second.cell);
            return scene == mCells.end() ? nullptr : scene->second.findObject(found->second.id);
        }

        const CellScene* findCell(const void* cellKey) const
        {
            const auto found = mCells.find(cellKey);
            return found == mCells.end() ? nullptr : &found->second;
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
            int gridY)
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
            ensureCell(newCellKey, exterior, gridX, gridY).objects.push_back(std::move(moved));

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
