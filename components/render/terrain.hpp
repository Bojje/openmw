#ifndef OPENMW_COMPONENTS_RENDER_TERRAIN_H
#define OPENMW_COMPONENTS_RENDER_TERRAIN_H

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "texture.hpp"

namespace Render
{
    struct TerrainVertex
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{};
        std::array<std::uint8_t, 4> color{ 255, 255, 255, 255 };
    };

    struct TerrainHeightField
    {
        std::vector<float> heights;
        std::uint32_t verticesPerSide = 0;
        float minHeight = 0.f;
        float maxHeight = 0.f;

        bool valid() const
        {
            return verticesPerSide > 1 && heights.size() == static_cast<std::size_t>(verticesPerSide) * verticesPerSide
                && std::all_of(heights.begin(), heights.end(), [](float value) { return std::isfinite(value); })
                && std::isfinite(minHeight) && std::isfinite(maxHeight) && minHeight <= maxHeight;
        }
    };

    struct TerrainLayer
    {
        std::string diffuseTexture;
        std::string normalTexture;
        std::string specularTexture;
        bool parallax = false;
        bool specular = false;
        TextureData blendmap;
    };

    struct TerrainTile
    {
        int lod = 0;
        float size = 0.f;
        std::array<float, 2> center{};
        float cellWorldSize = 0.f;
        float blendmapScale = 1.f;
        std::uint32_t verticesPerSide = 0;
        std::vector<TerrainVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<TerrainLayer> layers;

        bool valid() const
        {
            if (lod < 0 || size <= 0.f || cellWorldSize <= 0.f || blendmapScale <= 0.f || verticesPerSide <= 1
                || vertices.size() != static_cast<std::size_t>(verticesPerSide) * verticesPerSide
                || indices.empty() || indices.size() % 3 != 0 || layers.empty())
                return false;

            if (!std::isfinite(size) || !std::isfinite(center[0]) || !std::isfinite(center[1])
                || !std::isfinite(cellWorldSize) || !std::isfinite(blendmapScale))
                return false;

            for (const TerrainVertex& vertex : vertices)
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
            }

            if (!std::all_of(indices.begin(), indices.end(), [this](std::uint32_t index) {
                    return index < vertices.size();
                }))
                return false;

            for (const TerrainLayer& layer : layers)
            {
                if (layer.diffuseTexture.empty())
                    return false;
                const bool hasBlendmap = layer.blendmap.width != 0 || layer.blendmap.height != 0
                    || !layer.blendmap.pixels.empty();
                if (hasBlendmap && !layer.blendmap.valid())
                    return false;
                if (layers.size() > 1 && !layer.blendmap.valid())
                    return false;
            }
            return true;
        }
    };

    // A renderer-neutral quadtree region. The region owns one or more tiles
    // for the same aligned square; the frame collector chooses one by camera
    // distance and falls back to cell snapshots when no complete region set
    // is available.
    struct TerrainRegion
    {
        int minCellX = 0;
        int maxCellX = -1;
        int minCellY = 0;
        int maxCellY = -1;
        std::vector<TerrainTile> lods;

        bool valid() const
        {
            const std::int64_t width = static_cast<std::int64_t>(maxCellX) - minCellX + 1;
            const std::int64_t height = static_cast<std::int64_t>(maxCellY) - minCellY + 1;
            if (minCellX > maxCellX || minCellY > maxCellY || width != height || width <= 0
                || (width & (width - 1)) != 0
                || lods.empty())
                return false;

            const float expectedCenterX = static_cast<float>(minCellX) + width / 2.f;
            const float expectedCenterY = static_cast<float>(minCellY) + width / 2.f;
            int expectedLod = 0;
            return std::all_of(lods.begin(), lods.end(), [&](const TerrainTile& tile) {
                const bool matchingRegion = std::abs(tile.size - static_cast<float>(width)) < 0.0001f
                    && std::abs(tile.center[0] - expectedCenterX) < 0.0001f
                    && std::abs(tile.center[1] - expectedCenterY) < 0.0001f && tile.lod == expectedLod;
                ++expectedLod;
                return matchingRegion && tile.valid();
            });
        }
    };
}

#endif
