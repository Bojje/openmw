#ifndef OPENMW_COMPONENTS_NIF_MESHCONVERTER_H
#define OPENMW_COMPONENTS_NIF_MESHCONVERTER_H

#include <components/render/mesh.hpp>

namespace Nif
{
    struct NiTriShapeData;

    Render::MeshData convertMesh(const NiTriShapeData& source);
}

#endif
