#ifndef OPENMW_COMPONENTS_RENDER_TERRAINPAGING_H
#define OPENMW_COMPONENTS_RENDER_TERRAINPAGING_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "terrain.hpp"

namespace Render
{
    // Choose one cached LOD for a loaded cell. The source owns the cache and
    // supplies tiles in ascending LOD order; the renderer receives only the
    // selected snapshot, so no backend needs to know about legacy paging.
    inline const TerrainTile* selectTerrainLod(
        const std::vector<TerrainTile>& tiles, float cameraX, float cameraY)
    {
        if (tiles.empty())
            return nullptr;

        const TerrainTile* result = &tiles.front();
        for (const TerrainTile& candidate : tiles)
        {
            const float dx = cameraX - candidate.center[0] * candidate.cellWorldSize;
            const float dy = cameraY - candidate.center[1] * candidate.cellWorldSize;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const std::size_t lod = static_cast<std::size_t>(std::max(candidate.lod, 0));
            const std::size_t shift = std::min<std::size_t>(lod, 30);
            const float switchDistance
                = candidate.size * candidate.cellWorldSize * static_cast<float>(1u << shift) * 16.f;
            if (distance >= switchDistance && candidate.lod >= result->lod)
                result = &candidate;
        }
        return result;
    }
}

#endif
