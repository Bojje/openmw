#ifndef OPENMW_COMPONENTS_RENDER_TERRAIN_H
#define OPENMW_COMPONENTS_RENDER_TERRAIN_H

#include <array>
#include <algorithm>
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

    struct TerrainLayer
    {
        std::string diffuseTexture;
        std::string normalTexture;
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
                || indices.empty() || indices.size() % 3 != 0)
                return false;

            return std::all_of(indices.begin(), indices.end(), [this](std::uint32_t index) {
                return index < vertices.size();
            });
        }
    };
}

#endif
