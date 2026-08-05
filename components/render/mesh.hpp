#ifndef OPENMW_COMPONENTS_RENDER_MESH_H
#define OPENMW_COMPONENTS_RENDER_MESH_H

#include <cstdint>
#include <vector>

#include "scene.hpp"

namespace Render
{
    struct MeshVertex
    {
        float position[3];
        float normal[3];
        float texcoord[2];
        float color[4];
    };

    static_assert(sizeof(MeshVertex) == sizeof(float) * 12);

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
    };

    struct MeshInstance
    {
        MeshData mesh;
        Mat4 transform;
    };
}

#endif
