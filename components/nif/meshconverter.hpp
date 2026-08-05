#ifndef OPENMW_COMPONENTS_NIF_MESHCONVERTER_H
#define OPENMW_COMPONENTS_NIF_MESHCONVERTER_H

#include <vector>

#include <components/nif/niffile.hpp>
#include <components/render/mesh.hpp>

namespace Nif
{
    struct NiTriShapeData;

    Render::MeshData convertMesh(const NiTriShapeData& source);
    std::vector<Render::MeshData> collectMeshes(FileView file);
}

#endif
