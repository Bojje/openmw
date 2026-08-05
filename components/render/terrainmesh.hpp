#ifndef OPENMW_COMPONENTS_RENDER_TERRAINMESH_H
#define OPENMW_COMPONENTS_RENDER_TERRAINMESH_H

#include <memory>
#include <optional>
#include <vector>

#include "mesh.hpp"
#include "terrain.hpp"

namespace Render
{
    inline std::optional<MeshInstance> makeTerrainLayerMesh(
        const TerrainTile& tile, const TerrainLayer& layer, std::size_t layerIndex)
    {
        if (!tile.valid() || tile.layers.empty() || layer.diffuseTexture.empty())
            return std::nullopt;
        if (tile.layers.size() > 1 && !layer.blendmap.valid())
            return std::nullopt;

        const std::uint32_t side = tile.verticesPerSide;
        MeshInstance result;
        result.mesh.vertices.resize(tile.vertices.size());
        result.mesh.indices = tile.indices;
        result.mesh.material.albedoTexture = layer.diffuseTexture;
        result.mesh.material.normalTexture = layer.normalTexture;
        result.mesh.material.terrainBlend = layer.blendmap.valid();
        result.mesh.material.terrainFirstLayer = layerIndex == 0;
        result.mesh.material.terrainNormalMap = !layer.normalTexture.empty();
        result.mesh.material.terrainParallax = result.mesh.material.terrainNormalMap && layer.parallax;
        if (layer.blendmap.valid())
        {
            result.mesh.material.alphaBlend = true;
            result.mesh.material.alphaTexture = std::make_shared<const TextureData>(layer.blendmap);
        }

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
            const float u = static_cast<float>(x) / static_cast<float>(side - 1);
            const float v = static_cast<float>(y) / static_cast<float>(side - 1);
            target.texcoord[0] = u * tile.size;
            target.texcoord[1] = v * tile.size;
            target.blendTexcoord[0] = u;
            target.blendTexcoord[1] = v;
            if (layer.blendmap.valid())
            {
                const float scale = tile.blendmapScale / (tile.blendmapScale + 1.f);
                target.blendTexcoord[0] = scale * u
                    + 0.5f * (1.f - scale) + 1.f / tile.blendmapScale / 4.f;
                target.blendTexcoord[1] = scale * v
                    + 0.5f * (1.f - scale) - 1.f / tile.blendmapScale / 4.f;
            }
            for (std::size_t channel = 0; channel < 4; ++channel)
                target.color[channel] = static_cast<float>(source.color[channel]) / 255.f;
            target.material[0] = 1.f;
            target.material[1] = 0.f;
            target.material[2] = 1.f;
            target.material[3] = 0.f;
        }

        return result;
    }

    // Terrain layers are ordered like the legacy passes: the first layer
    // establishes depth and subsequent layers use equal-depth additive
    // blending. A missing blendmap in a multi-layer tile is rejected instead
    // of silently turning a layer opaque.
    inline std::vector<MeshInstance> makeTerrainMeshes(const TerrainTile& tile)
    {
        std::vector<MeshInstance> result;
        if (!tile.valid() || tile.layers.empty())
            return result;

        result.reserve(tile.layers.size());
        for (std::size_t layerIndex = 0; layerIndex < tile.layers.size(); ++layerIndex)
        {
            const auto mesh = makeTerrainLayerMesh(tile, tile.layers[layerIndex], layerIndex);
            if (!mesh)
                return {};
            result.push_back(*mesh);
        }
        return result;
    }

}

#endif
