#ifndef OPENMW_COMPONENTS_RENDER_WORLD_H
#define OPENMW_COMPONENTS_RENDER_WORLD_H

#include <cstdint>
#include <string>
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
    };

    // A cell snapshot is updated by the world lifecycle, not by a renderer.
    struct CellScene
    {
        bool exterior = false;
        int gridX = 0;
        int gridY = 0;
        std::vector<WorldObject> objects;
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
