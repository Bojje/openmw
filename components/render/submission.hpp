#ifndef OPENMW_COMPONENTS_RENDER_SUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_SUBMISSION_H

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mesh.hpp"
#include "scene.hpp"
#include "terrain.hpp"
#include "terrainpaging.hpp"
#include "texture.hpp"

namespace Render
{
    inline bool validMeshInstance(const MeshInstance& instance, bool allowEmptyIndices)
    {
        if (instance.mesh.indices.empty())
            return allowEmptyIndices;
        if (instance.mesh.vertices.empty() || !Render::valid(instance.transform)
            || !Render::valid(instance.mesh.material.diffuse) || !Render::valid(instance.mesh.material.emissive)
            || !std::isfinite(instance.mesh.material.glossiness)
            || (instance.mesh.skinning && !instance.mesh.skinning->valid(instance.mesh.vertices.size())))
            return false;

        for (const MeshVertex& vertex : instance.mesh.vertices)
        {
            for (const float value : vertex.position)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.normal)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.texcoord)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.blendTexcoord)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.color)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.material)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.tangent)
                if (!std::isfinite(value))
                    return false;
        }
        return std::all_of(instance.mesh.indices.begin(), instance.mesh.indices.end(), [&](std::uint32_t index) {
            return index < instance.mesh.vertices.size();
        });
    }

    struct DynamicMeshSubmission
    {
        WorldObject object;
        std::vector<MeshInstance> meshes;
    };

    // One backend-neutral frame submission. Resource resolution remains a
    // callback because resource ownership belongs to the game/resource layer;
    // the renderer receives no OSG scene objects.
    struct SceneSubmission
    {
        SceneData scene;
        std::vector<MeshInstance> meshes;
        std::vector<TerrainTile> terrainTiles;
        std::vector<std::string> unresolvedModels;
        // Dynamic records and their resolved meshes cross the frame boundary
        // together, so a future animation backend can consume skinning data
        // without borrowing WorldScene storage or re-resolving assets.
        std::vector<DynamicMeshSubmission> dynamicMeshes;
        TextureResolver textureResolver;

        bool valid() const
        {
            if (!scene.valid())
                return false;
            if (!unresolvedModels.empty())
                return false;

            for (const MeshInstance& instance : meshes)
            {
                if (!validMeshInstance(instance, true))
                    return false;
            }

            for (const DynamicMeshSubmission& dynamic : dynamicMeshes)
            {
                if (!dynamic.object.dynamic || dynamic.object.model.empty() || !dynamic.object.transform.valid())
                    return false;
                for (const MeshInstance& instance : dynamic.meshes)
                {
                    if (!validMeshInstance(instance, false))
                        return false;
                }
            }

            for (const DynamicMeshSubmission& dynamic : dynamicMeshes)
            {
                if (!dynamic.object.dynamic || dynamic.object.model.empty() || !dynamic.object.transform.valid())
                    return false;
            }

            return std::all_of(terrainTiles.begin(), terrainTiles.end(), [](const TerrainTile& tile) {
                return tile.valid();
            });
        }

        std::vector<std::string> referencedTexturePaths() const
        {
            std::unordered_set<std::string> seen;
            std::vector<std::string> result;
            const auto add = [&](std::string_view path) {
                if (!path.empty() && seen.emplace(path).second)
                    result.emplace_back(path);
            };
            const auto addMesh = [&add](const MeshInstance& instance) {
                add(instance.mesh.material.albedoTexture);
                add(instance.mesh.material.normalTexture);
                add(instance.mesh.material.emissiveTexture);
                add(instance.mesh.material.specularTexture);
            };
            for (const MeshInstance& instance : meshes)
                addMesh(instance);
            for (const DynamicMeshSubmission& dynamic : dynamicMeshes)
                for (const MeshInstance& instance : dynamic.meshes)
                    addMesh(instance);
            for (const TerrainTile& tile : terrainTiles)
                for (const TerrainLayer& layer : tile.layers)
                {
                    add(layer.diffuseTexture);
                    add(layer.normalTexture);
                    add(layer.specularTexture);
                }
            return result;
        }

        std::string validationError() const
        {
            if (!valid())
                return "invalid geometry";
            if (!textureResolver)
                return "no texture resolver";
            for (const std::string& path : referencedTexturePaths())
            {
                const std::shared_ptr<const TextureData> texture = textureResolver(path);
                if (!texture || !texture->valid())
                    return "invalid texture resource for '" + path + "'";
            }
            return {};
        }
    };

    inline std::vector<MeshInstance> collectUnskinnedDynamicMeshes(const SceneSubmission& submission)
    {
        std::vector<MeshInstance> result;
        for (const DynamicMeshSubmission& dynamic : submission.dynamicMeshes)
        {
            if (!dynamic.object.visible)
                continue;
            for (const MeshInstance& instance : dynamic.meshes)
            {
                if (!instance.mesh.skinning)
                    result.push_back(instance);
            }
        }
        return result;
    }

    // Build the backend-neutral portion of a frame from the scene owner. The
    // resource resolver remains supplied by the game layer, while mesh and
    // terrain collection stay independent of any renderer implementation.
    template <class ResolveMeshes>
    SceneSubmission collectSceneSubmission(const WorldScene& world, const SceneData& scene,
        std::string_view worldspace, ResolveMeshes&& resolveMeshes, bool includeTerrain = true)
    {
        SceneSubmission result;
        result.scene = scene;
        result.meshes = collectWorldMeshes(world, resolveMeshes, worldspace, &result.unresolvedModels);
        for (const CellScene* cell : world.cellsInOrder(worldspace))
            for (const WorldObject& object : cell->objects)
            {
                if (!object.dynamic)
                    continue;
                DynamicMeshSubmission dynamic;
                dynamic.object = object;
                if (object.visible)
                {
                    const std::vector<MeshInstance> resolvedMeshes = resolveMeshes(object.model);
                    if (resolvedMeshes.empty())
                        result.unresolvedModels.push_back(object.model);
                    for (const MeshInstance& mesh : resolvedMeshes)
                        dynamic.meshes.push_back(transformMeshInstance(object, mesh));
                }
                result.dynamicMeshes.push_back(std::move(dynamic));
            }

        if (includeTerrain)
        {
            const float cameraX = scene.viewInverse.data[12];
            const float cameraY = scene.viewInverse.data[13];
            for (const CellScene* cell : world.cellsInOrder(worldspace))
            {
                if (!cell->exterior || cell->terrainTiles.empty())
                    continue;
                if (const TerrainTile* selected = selectTerrainLod(cell->terrainTiles, cameraX, cameraY))
                    result.terrainTiles.push_back(*selected);
            }
        }
        return result;
    }
}

#endif
