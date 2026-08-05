#ifndef OPENMW_COMPONENTS_NIF_MESHCONVERTER_H
#define OPENMW_COMPONENTS_NIF_MESHCONVERTER_H

#include <vector>

#include <components/nif/niffile.hpp>
#include <components/render/mesh.hpp>

namespace Nif
{
    struct NiTriShapeData;
    struct NiTriStripsData;

    Render::MeshData convertMesh(const NiTriShapeData& source);
    Render::MeshData convertMesh(const NiTriStripsData& source);
    std::vector<Render::MeshInstance> collectMeshInstances(FileView file);
}

#endif
