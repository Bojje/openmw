#ifndef OPENMW_COMPONENTS_NIF_MESHCONVERTER_H
#define OPENMW_COMPONENTS_NIF_MESHCONVERTER_H

#include <vector>
#include <span>
#include <string>

#include <components/nif/niffile.hpp>
#include <components/render/mesh.hpp>

namespace Nif
{
    struct NiTriShapeData;
    struct NiTriStripsData;

    Render::MeshData convertMesh(const NiTriShapeData& source);
    Render::MeshData convertMesh(const NiTriStripsData& source);
    std::vector<Render::MeshInstance> collectMeshInstances(FileView file);

    /// Sample model-local bone transforms without constructing an OSG scene.
    /// The returned matrices are ordered like a boneNames and are suitable as
    /// the current bone matrices passed to Render::skinMesh.
    std::vector<Render::Mat4> collectBonePose(
        FileView file, std::span<const std::string> boneNames, float time);
}

#endif
