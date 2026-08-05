#ifndef OPENMW_COMPONENTS_RENDER_MESH_H
#define OPENMW_COMPONENTS_RENDER_MESH_H

#include <cstdint>
#include <algorithm>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "math.hpp"
#include "texture.hpp"
#include "world.hpp"

namespace Render
{
    struct MeshMaterial
    {
        std::string albedoTexture;
        std::string normalTexture;
        Vec4 diffuse{ 1.f, 1.f, 1.f, 1.f };
        Vec4 emissive{};
        float glossiness = 0.f;
        bool alphaBlend = false;
        bool alphaTest = false;
        uint8_t alphaTestThreshold = 0;
        bool terrainBlend = false;
        bool terrainFirstLayer = false;
        bool terrainNormalMap = false;
        bool terrainParallax = false;
        bool terrainSpecular = false;
        std::shared_ptr<const TextureData> alphaTexture;
    };

    struct MeshVertex
    {
        float position[3];
        float normal[3];
        float texcoord[2];
        float blendTexcoord[2];
        float color[4];
        float material[4];
    };

    static_assert(sizeof(MeshVertex) == sizeof(float) * 18);

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        MeshMaterial material;
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
        MeshMaterial material;
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
                mesh.mesh.material,
            };
            for (uint32_t index : mesh.mesh.indices)
            {
                if (index >= mesh.mesh.vertices.size()
                    || static_cast<uint64_t>(index) + vertexOffset > std::numeric_limits<uint32_t>::max())
                    throw std::runtime_error("Mesh batch contains an invalid index");
                result.indices.push_back(index + vertexOffset);
            }
            for (const MeshVertex& source : mesh.mesh.vertices)
            {
                MeshVertex vertex = source;
                vertex.color[0] *= mesh.mesh.material.diffuse.x;
                vertex.color[1] *= mesh.mesh.material.diffuse.y;
                vertex.color[2] *= mesh.mesh.material.diffuse.z;
                vertex.color[3] *= mesh.mesh.material.diffuse.w;
                vertex.material[0] = std::clamp(1.f - mesh.mesh.material.glossiness / 128.f, 0.f, 1.f);
                vertex.material[1] = 0.f;
                vertex.material[2] = mesh.mesh.material.terrainSpecular ? 2.f : 1.f;
                vertex.material[3] = std::max({ mesh.mesh.material.emissive.x, mesh.mesh.material.emissive.y,
                    mesh.mesh.material.emissive.z });
                result.vertices.push_back(vertex);
            }
            result.draws.push_back(draw);
        }
        return result;
    }

    inline MeshInstance transformMeshInstance(const WorldObject& object, const MeshInstance& mesh)
    {
        MeshInstance result = mesh;
        result.transform = multiply(makeObjectTransformMatrix(object.transform), mesh.transform);
        return result;
    }

    template <class ResolveMeshes>
    std::vector<MeshInstance> collectCellMeshes(const CellScene& scene, ResolveMeshes&& resolveMeshes)
    {
        std::vector<MeshInstance> result;
        for (const WorldObject& object : scene.objects)
        {
            if (!object.visible)
                continue;

            for (const MeshInstance& mesh : resolveMeshes(object.model))
                result.push_back(transformMeshInstance(object, mesh));
        }
        return result;
    }

    template <class ResolveMeshes>
    std::vector<MeshInstance> collectWorldMeshes(
        const WorldScene& world, ResolveMeshes&& resolveMeshes, std::string_view worldspace = {})
    {
        std::vector<MeshInstance> result;
        for (const CellScene* cell : world.cellsInOrder(worldspace))
        {
            std::vector<MeshInstance> cellMeshes = collectCellMeshes(*cell, resolveMeshes);
            result.insert(result.end(), std::make_move_iterator(cellMeshes.begin()),
                std::make_move_iterator(cellMeshes.end()));
        }
        return result;
    }

    // Produce a deterministic draw order for a backend that uses a single
    // depth-tested pass. Terrain layers are kept in submission order because
    // their first/equal-depth pipelines encode legacy blending semantics;
    // ordinary alpha-blended draws are sorted back-to-front.
    inline std::vector<std::size_t> orderMeshDraws(
        const std::vector<MeshDraw>& draws, const Vec3& cameraPosition)
    {
        std::vector<std::size_t> opaque;
        std::vector<std::size_t> terrain;
        std::vector<std::size_t> transparent;
        opaque.reserve(draws.size());
        terrain.reserve(draws.size());
        transparent.reserve(draws.size());

        for (std::size_t index = 0; index < draws.size(); ++index)
        {
            if (draws[index].material.terrainBlend)
                terrain.push_back(index);
            else if (draws[index].material.alphaBlend)
                transparent.push_back(index);
            else
                opaque.push_back(index);
        }

        const auto distanceSquared = [&](std::size_t index) {
            const MeshDraw& draw = draws[index];
            const float dx = draw.transform.data[12] - cameraPosition.x;
            const float dy = draw.transform.data[13] - cameraPosition.y;
            const float dz = draw.transform.data[14] - cameraPosition.z;
            return dx * dx + dy * dy + dz * dz;
        };
        std::stable_sort(transparent.begin(), transparent.end(), [&](std::size_t lhs, std::size_t rhs) {
            return distanceSquared(lhs) > distanceSquared(rhs);
        });

        std::vector<std::size_t> result;
        result.reserve(draws.size());
        result.insert(result.end(), opaque.begin(), opaque.end());
        result.insert(result.end(), terrain.begin(), terrain.end());
        result.insert(result.end(), transparent.begin(), transparent.end());
        return result;
    }
}

#endif
