#include "renderstorage.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <utility>

namespace Terrain
{
    namespace
    {
        int maxTerrainLod(int cellVertices)
        {
            int maxLod = 0;
            for (int vertices = std::max(cellVertices - 1, 1); vertices > 1; vertices >>= 1)
                ++maxLod;
            return maxLod;
        }

        int largestAlignedRegion(int x, int y, int maxCellX, int maxCellY,
            const std::set<std::pair<int, int>>& activeCells,
            const std::set<std::pair<int, int>>& covered)
        {
            int size = 1;
            while (size <= (maxCellX - x) && size <= (maxCellY - y)
                && x % (size * 2) == 0 && y % (size * 2) == 0)
            {
                const int candidate = size * 2;
                bool complete = true;
                for (int cellY = y; cellY < y + candidate && complete; ++cellY)
                    for (int cellX = x; cellX < x + candidate; ++cellX)
                        if (!activeCells.contains({ cellX, cellY }) || covered.contains({ cellX, cellY }))
                        {
                            complete = false;
                            break;
                        }
                if (!complete)
                    break;
                size = candidate;
            }
            return size;
        }

        std::vector<Render::TerrainRegion> assembleRenderRegionTiles(int minCellX, int maxCellX,
            int minCellY, int maxCellY, const std::set<std::pair<int, int>>& activeCells,
            Terrain::RenderStorage& storage, ESM::RefId worldspace)
        {
            std::vector<Render::TerrainRegion> result;
            if (minCellX > maxCellX || minCellY > maxCellY)
                return result;

            const int maxLod = maxTerrainLod(storage.getCellVertices(worldspace));
            std::set<std::pair<int, int>> covered;
            for (int y = minCellY; y <= maxCellY; ++y)
            {
                for (int x = minCellX; x <= maxCellX; ++x)
                {
                    if (!activeCells.contains({ x, y }) || covered.contains({ x, y }))
                        continue;

                    const int size = largestAlignedRegion(x, y, maxCellX, maxCellY, activeCells, covered);
                    Render::TerrainRegion& region = result.emplace_back();
                    region.minCellX = x;
                    region.maxCellX = x + size - 1;
                    region.minCellY = y;
                    region.maxCellY = y + size - 1;

                    const std::array<float, 2> center = { x + size / 2.f, y + size / 2.f };
                    for (int lod = 0; lod <= maxLod; ++lod)
                    {
                        const std::optional<Render::TerrainTile> tile
                            = storage.getRenderTile(lod, static_cast<float>(size), center, worldspace);
                        if (!tile)
                            break;
                        region.lods.push_back(*tile);
                    }

                    for (int coveredY = y; coveredY < y + size; ++coveredY)
                        for (int coveredX = x; coveredX < x + size; ++coveredX)
                            covered.emplace(coveredX, coveredY);
                }
            }
            return result;
        }
    }

    std::vector<Render::TerrainTile> RenderStorage::getRenderTiles(int gridX, int gridY, ESM::RefId worldspace)
    {
        const std::array<float, 2> center = { gridX + 0.5f, gridY + 0.5f };
        const int maxLod = maxTerrainLod(getCellVertices(worldspace));

        std::vector<Render::TerrainTile> tiles;
        tiles.reserve(static_cast<std::size_t>(maxLod + 1));
        for (int lod = 0; lod <= maxLod; ++lod)
        {
            if (std::optional<Render::TerrainTile> tile = getRenderTile(lod, 1.f, center, worldspace))
                tiles.push_back(std::move(*tile));
            else
                break;
        }
        return tiles;
    }

    std::vector<Render::TerrainRegion> RenderStorage::getRenderRegionTiles(
        int minCellX, int maxCellX, int minCellY, int maxCellY, ESM::RefId worldspace)
    {
        std::set<std::pair<int, int>> activeCells;
        for (int y = minCellY; y <= maxCellY; ++y)
            for (int x = minCellX; x <= maxCellX; ++x)
                activeCells.emplace(x, y);
        return getRenderRegionTiles(activeCells, worldspace);
    }

    std::vector<Render::TerrainRegion> RenderStorage::getRenderRegionTiles(
        const std::set<std::pair<int, int>>& activeCells, ESM::RefId worldspace)
    {
        if (activeCells.empty())
            return {};

        int minCellX = activeCells.begin()->first;
        int maxCellX = minCellX;
        int minCellY = activeCells.begin()->second;
        int maxCellY = minCellY;
        for (const auto [cellX, cellY] : activeCells)
        {
            minCellX = std::min(minCellX, cellX);
            maxCellX = std::max(maxCellX, cellX);
            minCellY = std::min(minCellY, cellY);
            maxCellY = std::max(maxCellY, cellY);
        }
        return assembleRenderRegionTiles(minCellX, maxCellX, minCellY, maxCellY, activeCells, *this, worldspace);
    }

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
