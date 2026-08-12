#ifndef OPENMW_COMPONENTS_RENDER_MESHCONVERSION_H
#define OPENMW_COMPONENTS_RENDER_MESHCONVERSION_H

#include <cstdint>

#include "mesh.hpp"

namespace Render
{
    void appendMeshIndex(MeshData& mesh, std::uint32_t index);
    void computeMeshTangents(MeshData& mesh);
}

#endif
