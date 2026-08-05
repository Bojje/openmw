#ifndef OPENMW_COMPONENTS_RENDER_TERRAINMESH_H
#define OPENMW_COMPONENTS_RENDER_TERRAINMESH_H

#include <optional>

#include "mesh.hpp"
#include "terrain.hpp"

namespace Render
{
    // The first Vulkan terrain consumer intentionally accepts only the opaque
    // single-layer form. Multi-layer blendmaps need a terrain-specific shader
    // and remain in the deletion ledger until that consumer exists.
    inline std::optional<MeshInstance> makeOpaqueTerrainMesh(const TerrainTile& tile)
    {
        if (!tile.valid() || tile.layers.size() != 1 || tile.layers[0].blendmap.valid())
            return std::nullopt;

        const std::uint32_t side = tile.verticesPerSide;
        MeshInstance result;
        result.mesh.vertices.resize(tile.vertices.size());
        result.mesh.indices = tile.indices;
        result.mesh.material.albedoTexture = tile.layers[0].diffuseTexture;

        result.transform = {};
        result.transform.data[0] = 1.f;
        result.transform.data[5] = 1.f;
        result.transform.data[10] = 1.f;
        result.transform.data[15] = 1.f;
        result.transform.data[12] = tile.center[0] * tile.cellWorldSize;
        result.transform.data[13] = tile.center[1] * tile.cellWorldSize;

        for (std::size_t index = 0; index < tile.vertices.size(); ++index)
        {
            const TerrainVertex& source = tile.vertices[index];
            MeshVertex& target = result.mesh.vertices[index];
            target.position[0] = source.position[0];
            target.position[1] = source.position[1];
            target.position[2] = source.position[2];
            target.normal[0] = source.normal[0];
            target.normal[1] = source.normal[1];
            target.normal[2] = source.normal[2];
            const std::uint32_t x = static_cast<std::uint32_t>(index) % side;
            const std::uint32_t y = static_cast<std::uint32_t>(index) / side;
            target.texcoord[0] = static_cast<float>(x) / static_cast<float>(side - 1);
            target.texcoord[1] = static_cast<float>(y) / static_cast<float>(side - 1);
            for (std::size_t channel = 0; channel < 4; ++channel)
                target.color[channel] = static_cast<float>(source.color[channel]) / 255.f;
            target.material[0] = 1.f;
            target.material[1] = 0.f;
            target.material[2] = 1.f;
            target.material[3] = 0.f;
        }

        return result;
    }
}

#endif
