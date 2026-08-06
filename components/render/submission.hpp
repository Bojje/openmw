#ifndef OPENMW_COMPONENTS_RENDER_SUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_SUBMISSION_H

#include <algorithm>
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
            for (const MeshInstance& instance : meshes)
            {
                if (instance.mesh.indices.empty())
                    continue;
                if (instance.mesh.vertices.empty())
                    return false;
                for (const std::uint32_t index : instance.mesh.indices)
                {
                    if (index >= instance.mesh.vertices.size())
                        return false;
                }
            }

            return std::all_of(terrainTiles.begin(), terrainTiles.end(), [](const TerrainTile& tile) {
                return tile.valid();
            });
        }
    };
}

#endif
