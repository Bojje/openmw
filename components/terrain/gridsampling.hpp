#ifndef OPENMW_COMPONENTS_TERRAIN_GRIDSAMPLING_H
#define OPENMW_COMPONENTS_TERRAIN_GRIDSAMPLING_H

// The sampling routines are pure integer/grid logic. Keep this renderer-
// neutral include as the public home for new consumers while the legacy
// ESMTerrain include remains as a compatibility façade for existing tests and
// the reference storage implementation.
#include <components/esmterrain/gridsampling.hpp>

namespace Terrain
{
    using ESMTerrain::CellSample;
    using ESMTerrain::getBlendmapLocalRange;
    using ESMTerrain::getBlendmapSize;
    using ESMTerrain::sampleBlendmaps;
    using ESMTerrain::sampleCellGrid;
    using ESMTerrain::sampleCellGridSimple;
    using ESMTerrain::sampleGrid;
    using ESMTerrain::toCellAndLocal;
}

#endif
