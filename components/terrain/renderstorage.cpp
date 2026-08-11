#include "renderstorage.hpp"

#include <cmath>
#include <optional>

namespace Terrain
{
    std::optional<Render::TerrainTile> RenderStorage::getRenderTile(
        int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace)
    {
        if (lodLevel < 0 || size <= 0.f)
            return std::nullopt;

        std::vector<Render::TerrainVertex> vertices;
        fillRenderVertexBuffers(lodLevel, size, center, worldspace, vertices);

        Render::TerrainTile tile;
        tile.lod = lodLevel;
        tile.size = size;
        tile.center = center;
        tile.cellWorldSize = getCellWorldSize(worldspace);
        if (tile.cellWorldSize <= 0.f)
            return std::nullopt;
        tile.blendmapScale = static_cast<float>(getTextureTileCount(size, worldspace));
        if (tile.blendmapScale <= 0.f)
            return std::nullopt;
        tile.verticesPerSide = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(vertices.size())));
        while (static_cast<std::size_t>(tile.verticesPerSide + 1) * (tile.verticesPerSide + 1) <= vertices.size())
            ++tile.verticesPerSide;
        while (static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide > vertices.size())
            --tile.verticesPerSide;
        if (tile.verticesPerSide < 2
            || static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide != vertices.size())
            return std::nullopt;
        tile.vertices = std::move(vertices);
        tile.indices.reserve(static_cast<std::size_t>(tile.verticesPerSide - 1)
            * (tile.verticesPerSide - 1) * 6);
        for (std::uint32_t y = 0; y + 1 < tile.verticesPerSide; ++y)
        {
            for (std::uint32_t x = 0; x + 1 < tile.verticesPerSide; ++x)
            {
                const std::uint32_t topLeft = y * tile.verticesPerSide + x;
                const std::uint32_t topRight = topLeft + 1;
                const std::uint32_t bottomLeft = topLeft + tile.verticesPerSide;
                const std::uint32_t bottomRight = bottomLeft + 1;
                tile.indices.insert(tile.indices.end(), { topLeft, bottomLeft, topRight,
                    topRight, bottomLeft, bottomRight });
            }
        }

        std::vector<Render::TextureData> blendmaps;
        std::vector<LayerInfo> layerList;
        getRenderBlendmaps(size, center, blendmaps, layerList, worldspace);
        // A single opaque layer intentionally has no blendmap in the legacy
        // storage contract. Preserve that layer while leaving its neutral
        // blendmap invalid; the Vulkan consumer can treat it as fully opaque.
        if (!blendmaps.empty() && blendmaps.size() != layerList.size())
            return std::nullopt;

        tile.layers.reserve(layerList.size());
        for (std::size_t i = 0; i < layerList.size(); ++i)
        {
            Render::TerrainLayer& layer = tile.layers.emplace_back();
            layer.diffuseTexture = layerList[i].mDiffuseMap.value();
            layer.normalTexture = layerList[i].mNormalMap.value();
            layer.specularTexture = layerList[i].mSpecularMap.value();
            layer.parallax = layerList[i].mParallax;
            layer.specular = layerList[i].mSpecular;
            if (i < blendmaps.size())
            {
                layer.blendmap = std::move(blendmaps[i]);
                if (!layer.blendmap.valid())
                    return std::nullopt;
            }
        }
        return tile;
    }
}
