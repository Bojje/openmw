#ifndef OPENMW_COMPONENTS_RENDER_MESH_H
#define OPENMW_COMPONENTS_RENDER_MESH_H

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "math.hpp"
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

    struct MeshDraw
    {
        uint32_t indexCount;
        uint32_t firstIndex;
        int32_t vertexOffset;
        Mat4 transform;
        Mat4 normalMatrix;
    };

    struct MeshBatch
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshDraw> draws;
    };

    /// Flatten independent mesh instances into one indexed batch for a renderer backend.
    inline MeshBatch batchMeshes(const std::vector<MeshInstance>& meshes)
    {
        MeshBatch result;
        for (const MeshInstance& mesh : meshes)
        {
            if (mesh.mesh.vertices.empty() || mesh.mesh.indices.empty())
                continue;

            if (result.vertices.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
                throw std::runtime_error("Mesh batch has too many vertices");
            if (result.indices.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())
                || mesh.mesh.indices.size() > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max()))
                throw std::runtime_error("Mesh batch has too many indices");

            const uint32_t vertexOffset = static_cast<uint32_t>(result.vertices.size());
            MeshDraw draw = {
                static_cast<uint32_t>(mesh.mesh.indices.size()),
                static_cast<uint32_t>(result.indices.size()),
                static_cast<int32_t>(vertexOffset),
                mesh.transform,
                computeNormalMatrix(mesh.transform),
            };
            for (uint32_t index : mesh.mesh.indices)
            {
                if (index >= mesh.mesh.vertices.size()
                    || static_cast<uint64_t>(index) + vertexOffset > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Mesh batch contains an invalid index");
                result.indices.push_back(index + vertexOffset);
            }
            result.vertices.insert(result.vertices.end(), mesh.mesh.vertices.begin(), mesh.mesh.vertices.end());
            result.draws.push_back(draw);
        }
        return result;
    }
}

#endif
