#ifndef OPENMW_COMPONENTS_ESMTERRAIN_GRIDSAMPLING_H
#define OPENMW_COMPONENTS_ESMTERRAIN_GRIDSAMPLING_H

#include <components/terrain/gridsampling.hpp>

namespace ESMTerrain
{
    using Terrain::CellSample;
    using Terrain::getBlendmapLocalRange;
    using Terrain::getBlendmapSize;
    using Terrain::sampleBlendmaps;
    using Terrain::sampleCellGrid;
    using Terrain::sampleCellGridSimple;
    using Terrain::sampleGrid;
    using Terrain::toCellAndLocal;
}

#endif
