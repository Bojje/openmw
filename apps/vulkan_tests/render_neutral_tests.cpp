#include <cmath>
#include <stdexcept>

#include <components/render/meshconversion.hpp>

int main()
{
    std::vector<Render::MeshVertexSource> source(3);
    source[1].position[0] = 1.f;
    source[1].texcoord[0] = 1.f;
    source[2].position[1] = 1.f;
    source[2].texcoord[1] = 1.f;
    for (Render::MeshVertexSource& vertex : source)
        vertex.hasNormal = true;

    Render::MeshData mesh = Render::makeMeshData(source);

    Render::appendMeshIndex(mesh, 0);
    Render::appendMeshIndex(mesh, 1);
    Render::appendMeshIndex(mesh, 2);
    Render::computeMeshTangents(mesh);

    if (mesh.indices != std::vector<uint32_t>({ 0, 1, 2 })
        || std::abs(mesh.vertices[0].tangent[0] - 1.f) > 1e-5f
        || std::abs(mesh.vertices[0].tangent[1]) > 1e-5f
        || std::abs(mesh.vertices[0].tangent[2]) > 1e-5f
        || std::abs(mesh.vertices[0].tangent[3] - 1.f) > 1e-5f)
        throw std::runtime_error("neutral mesh tangent conversion is incorrect");

    bool rejectedInvalidIndex = false;
    try
    {
        Render::appendMeshIndex(mesh, 3);
    }
    catch (const std::runtime_error&)
    {
        rejectedInvalidIndex = true;
    }
    if (!rejectedInvalidIndex)
        throw std::runtime_error("neutral mesh conversion accepted an invalid index");
}
