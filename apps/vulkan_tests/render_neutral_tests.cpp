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

    Render::MeshData stripMesh = Render::makeMeshData(std::vector<Render::MeshVertexSource>(4));
    Render::appendTriangleStripIndices(stripMesh, std::vector<uint16_t>{ 0, 1, 2, 3 });
    if (stripMesh.indices != std::vector<uint32_t>({ 0, 1, 2, 1, 3, 2 }))
        throw std::runtime_error("neutral triangle-strip conversion returned the wrong winding");
}
