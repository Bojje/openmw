#ifndef OPENMW_COMPONENTS_RENDER_SUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_SUBMISSION_H

#include <algorithm>
#include <cmath>
#include <string_view>
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
        // Dynamic records are copied into the submission so a backend can
        // retain a frame payload without borrowing WorldScene storage. They
        // are not part of the static mesh batch until a skinning/animation
        // consumer is available.
        std::vector<WorldObject> dynamicObjects;
        // Resolved dynamic meshes are carried separately so a future
        // animation backend can consume skinning data without re-resolving
        // assets or falling back to static bind-pose rendering.
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

            for (const WorldObject& object : dynamicObjects)
            {
                if (!object.dynamic || object.model.empty() || !object.transform.valid())
                    return false;
            }
            if (dynamicMeshes.size() != dynamicObjects.size())
                return false;
            for (std::size_t index = 0; index < dynamicObjects.size(); ++index)
            {
                if (dynamicMeshes[index].object.id != dynamicObjects[index].id
                    || dynamicMeshes[index].object.model != dynamicObjects[index].model
                    || dynamicMeshes[index].object.dynamic != dynamicObjects[index].dynamic)
                    return false;
            }

            return std::all_of(terrainTiles.begin(), terrainTiles.end(), [](const TerrainTile& tile) {
                return tile.valid();
            });
        }
    };

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
        result.dynamicObjects = world.dynamicObjectsInOrder(worldspace);
        for (const WorldObject& object : result.dynamicObjects)
        {
            DynamicMeshSubmission dynamic;
            dynamic.object = object;
            const std::vector<MeshInstance> resolvedMeshes = resolveMeshes(object.model);
            if (resolvedMeshes.empty())
                result.unresolvedModels.push_back(object.model);
            for (const MeshInstance& mesh : resolvedMeshes)
                dynamic.meshes.push_back(transformMeshInstance(object, mesh));
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
