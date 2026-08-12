#ifndef OPENMW_COMPONENTS_RENDER_MESHCONVERSION_H
#define OPENMW_COMPONENTS_RENDER_MESHCONVERSION_H

#include <cstdint>
#include <array>
#include <span>

#include "mesh.hpp"

namespace Render
{
    struct MeshVertexSource
    {
        std::array<float, 3> position{};
        std::array<float, 3> normal{ 0.f, 0.f, 1.f };
        std::array<float, 2> texcoord{};
        std::array<float, 4> color{ 1.f, 1.f, 1.f, 1.f };
        bool hasNormal = false;
        bool hasTexcoord = false;
        bool hasColor = false;
    };

    MeshData makeMeshData(std::span<const MeshVertexSource> source);
    void appendMeshIndex(MeshData& mesh, std::uint32_t index);
    void appendTriangleStripIndices(MeshData& mesh, std::span<const std::uint16_t> strip);
    void computeMeshTangents(MeshData& mesh);
}

#endif
