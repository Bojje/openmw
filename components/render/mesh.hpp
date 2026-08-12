#ifndef OPENMW_COMPONENTS_RENDER_MESH_H
#define OPENMW_COMPONENTS_RENDER_MESH_H

#include <array>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "math.hpp"
#include "texture.hpp"
#include "world.hpp"

namespace Render
{
    inline std::string makeSpecularTexturePath(std::string_view albedoTexture, std::string_view pattern)
    {
        if (albedoTexture.empty() || pattern.empty())
            return {};

        std::string result(albedoTexture);
        const std::size_t extension = result.rfind('.');
        if (extension == std::string::npos)
            return {};
        result.replace(extension, 1, std::string(pattern) + '.');
        return result;
    }

    struct MeshMaterial
    {
        std::string albedoTexture;
        std::string normalTexture;
        std::string emissiveTexture;
        std::string specularTexture;
        Vec4 diffuse{ 1.f, 1.f, 1.f, 1.f };
        Vec4 emissive{};
        float glossiness = 0.f;
        bool alphaBlend = false;
        bool alphaTest = false;
        bool doubleSided = false;
        bool albedoWrapU = true;
        bool albedoWrapV = true;
        bool normalWrapU = true;
        bool normalWrapV = true;
        bool emissiveWrapU = true;
        bool emissiveWrapV = true;
        bool specularWrapU = true;
        bool specularWrapV = true;
        uint8_t alphaTestThreshold = 0;
        bool terrainBlend = false;
        bool terrainFirstLayer = false;
        bool normalMap = false;
        bool terrainNormalMap = false;
        bool terrainParallax = false;
        bool terrainSpecular = false;
        std::shared_ptr<const TextureData> alphaTexture;
    };

    struct SkinVertex
    {
        std::array<std::uint16_t, 4> boneIndices{};
        std::array<float, 4> weights{};
    };

    struct SkinningData
    {
        std::vector<SkinVertex> vertices;
        // Optional source names in the same order as inverseBindMatrices.
        // Keeping them here lets an animation producer map a pose by name
        // without exposing a scene-graph bone type to the renderer.
        std::vector<std::string> boneNames;
        std::vector<Mat4> inverseBindMatrices;

        bool valid(std::size_t vertexCount) const
        {
            if (vertices.size() != vertexCount || inverseBindMatrices.empty())
                return false;
            if (!boneNames.empty()
                && (boneNames.size() != inverseBindMatrices.size()
                    || std::any_of(boneNames.begin(), boneNames.end(), [](const std::string& name) {
                           return name.empty();
                       })))
                return false;

            for (const SkinVertex& vertex : vertices)
            {
                float weightSum = 0.f;
                for (std::size_t influence = 0; influence < vertex.weights.size(); ++influence)
                {
                    if (!std::isfinite(vertex.weights[influence]) || vertex.weights[influence] < 0.f
                        || vertex.boneIndices[influence] >= inverseBindMatrices.size())
                        return false;
                    weightSum += vertex.weights[influence];
                }
                if (!std::isfinite(weightSum) || weightSum <= 0.999f || weightSum > 1.001f)
                    return false;
            }

            return std::all_of(inverseBindMatrices.begin(), inverseBindMatrices.end(),
                [](const Mat4& matrix) { return Render::valid(matrix); });
        }
    };

    struct MeshVertex
    {
        float position[3];
        float normal[3];
        float texcoord[2];
        float blendTexcoord[2];
        float color[4];
        float material[4];
        float tangent[4];
    };

    static_assert(sizeof(MeshVertex) == sizeof(float) * 22);

    struct MeshData
    {
        std::vector<MeshVertex> vertices;
        std::vector<uint32_t> indices;
        MeshMaterial material;
        std::shared_ptr<const SkinningData> skinning;
    };

    // The game/resource layer owns animation state and exposes only the
    // renderer-neutral pose payload. A resolver may return an empty vector
    // when a model has no compatible model-local animation data; callers then
    // retain their bind-pose fallback.
    using PoseResolver = std::function<std::vector<Mat4>(
        std::string_view model, float time, std::span<const std::string> boneNames)>;

    inline MeshData skinMesh(const MeshData& source, std::span<const Mat4> boneMatrices)
    {
        if (!source.skinning || !source.skinning->valid(source.vertices.size()))
            throw std::invalid_argument("Cannot skin mesh without valid skinning data");
        if (boneMatrices.size() < source.skinning->inverseBindMatrices.size())
            throw std::invalid_argument("Bone matrix payload is smaller than the mesh skin");

        MeshData result = source;
        result.skinning.reset();
        for (std::size_t vertexIndex = 0; vertexIndex < result.vertices.size(); ++vertexIndex)
        {
            const MeshVertex sourceVertex = source.vertices[vertexIndex];
            MeshVertex& vertex = result.vertices[vertexIndex];
            std::array<float, 3> position{};
            std::array<float, 3> normal{};
            std::array<float, 3> tangent{};
            const SkinVertex& skin = source.skinning->vertices[vertexIndex];
            for (std::size_t influence = 0; influence < skin.weights.size(); ++influence)
            {
                const float weight = skin.weights[influence];
                if (weight <= 0.f)
                    continue;

                const Mat4 skinMatrix = multiply(boneMatrices[skin.boneIndices[influence]],
                    source.skinning->inverseBindMatrices[skin.boneIndices[influence]]);
                const Mat4 normalMatrix = computeNormalMatrix(skinMatrix);
                const auto addTransformed = [weight, &skinMatrix](std::array<float, 3>& target,
                                                const float* value, float homogeneous) {
                    target[0] += weight
                        * (skinMatrix.data[0] * value[0] + skinMatrix.data[4] * value[1]
                            + skinMatrix.data[8] * value[2] + skinMatrix.data[12] * homogeneous);
                    target[1] += weight
                        * (skinMatrix.data[1] * value[0] + skinMatrix.data[5] * value[1]
                            + skinMatrix.data[9] * value[2] + skinMatrix.data[13] * homogeneous);
                    target[2] += weight
                        * (skinMatrix.data[2] * value[0] + skinMatrix.data[6] * value[1]
                            + skinMatrix.data[10] * value[2] + skinMatrix.data[14] * homogeneous);
                };
                const auto addNormalTransformed = [weight, &normalMatrix](std::array<float, 3>& target,
                                                       const float* value) {
                    target[0] += weight
                        * (normalMatrix.data[0] * value[0] + normalMatrix.data[4] * value[1]
                            + normalMatrix.data[8] * value[2]);
                    target[1] += weight
                        * (normalMatrix.data[1] * value[0] + normalMatrix.data[5] * value[1]
                            + normalMatrix.data[9] * value[2]);
                    target[2] += weight
                        * (normalMatrix.data[2] * value[0] + normalMatrix.data[6] * value[1]
                            + normalMatrix.data[10] * value[2]);
                };
                addTransformed(position, sourceVertex.position, 1.f);
                addNormalTransformed(normal, sourceVertex.normal);
                addNormalTransformed(tangent, sourceVertex.tangent);
            }
            const auto normalize = [](std::array<float, 3>& value) {
                const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
                if (length > 1e-6f)
                    for (float& component : value)
                        component /= length;
            };
            normalize(normal);
            normalize(tangent);
            for (std::size_t axis = 0; axis < 3; ++axis)
            {
                vertex.position[axis] = position[axis];
                vertex.normal[axis] = normal[axis];
                vertex.tangent[axis] = tangent[axis];
            }
        }
        return result;
    }

    struct MeshInstance
    {
        MeshData mesh;
        Mat4 transform = identityMat4();
    };

    using MeshResolver = std::function<std::shared_ptr<const std::vector<MeshInstance>>(std::string_view)>;

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

    inline bool hasRenderableGeometry(const MeshInstance& instance)
    {
        return !instance.mesh.vertices.empty() && !instance.mesh.indices.empty();
    }

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
    std::vector<MeshInstance> collectCellMeshes(
        const CellScene& scene, ResolveMeshes&& resolveMeshes, std::vector<std::string>* unresolvedModels = nullptr)
    {
        std::vector<MeshInstance> result;
        for (const WorldObject& object : scene.objects)
        {
            if (!object.visible || object.dynamic)
                continue;

            const std::vector<MeshInstance> resolvedMeshes = resolveMeshes(object.model);
            const bool hasGeometry = std::any_of(resolvedMeshes.begin(), resolvedMeshes.end(), hasRenderableGeometry);
            if (!hasGeometry && unresolvedModels != nullptr)
                unresolvedModels->push_back(object.model);
            for (const MeshInstance& mesh : resolvedMeshes)
                result.push_back(transformMeshInstance(object, mesh));
        }
        return result;
    }

    template <class ResolveMeshes>
    std::vector<MeshInstance> collectWorldMeshes(
        const WorldScene& world, ResolveMeshes&& resolveMeshes, std::string_view worldspace = {},
        std::vector<std::string>* unresolvedModels = nullptr)
    {
        std::vector<MeshInstance> result;
        for (const CellScene* cell : world.cellsInOrder(worldspace))
        {
            std::vector<MeshInstance> cellMeshes = collectCellMeshes(*cell, resolveMeshes, unresolvedModels);
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
