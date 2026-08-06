#ifndef OPENMW_COMPONENTS_RENDER_SUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_SUBMISSION_H

#include <algorithm>
#include <cmath>
#include <vector>

#include "mesh.hpp"
#include "scene.hpp"
#include "terrain.hpp"
#include "texture.hpp"

namespace Render
{
    // One backend-neutral frame submission. Resource resolution remains a
    // callback because resource ownership belongs to the game/resource layer;
    // the renderer receives no OSG scene objects.
    struct SceneSubmission
    {
        SceneData scene;
        std::vector<MeshInstance> meshes;
        std::vector<TerrainTile> terrainTiles;
        // Dynamic records are copied into the submission so a backend can
        // retain a frame payload without borrowing WorldScene storage. They
        // are not part of the static mesh batch until a skinning/animation
        // consumer is available.
        std::vector<WorldObject> dynamicObjects;
        TextureResolver textureResolver;

        bool valid() const
        {
            if (!scene.valid())
                return false;

            for (const MeshInstance& instance : meshes)
            {
                if (instance.mesh.indices.empty())
                    continue;
                if (instance.mesh.vertices.empty() || !Render::valid(instance.transform)
                    || !Render::valid(instance.mesh.material.diffuse) || !Render::valid(instance.mesh.material.emissive)
                    || !std::isfinite(instance.mesh.material.glossiness))
                    return false;
                for (const MeshVertex& vertex : instance.mesh.vertices)
                {
                    for (const float value : vertex.position)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.normal)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.texcoord)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.blendTexcoord)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.color)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.material)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                    for (const float value : vertex.tangent)
                    {
                        if (!std::isfinite(value))
                            return false;
                    }
                }
                for (const std::uint32_t index : instance.mesh.indices)
                {
                    if (index >= instance.mesh.vertices.size())
                        return false;
                }
            }

            for (const WorldObject& object : dynamicObjects)
            {
                if (!object.dynamic || object.model.empty() || !object.transform.valid())
                    return false;
            }

            return std::all_of(terrainTiles.begin(), terrainTiles.end(), [](const TerrainTile& tile) {
                return tile.valid();
            });
        }
    };
}

#endif
