#include <cmath>
#include <iostream>
#include <stdexcept>

#include <components/nif/data.hpp>
#include <components/nif/meshconverter.hpp>

namespace
{
    void expectNear(float actual, float expected, const char* label)
    {
        if (std::abs(actual - expected) > 1e-5f)
            throw std::runtime_error(label);
    }
}

int main()
{
    Nif::NiTriShapeData source;
    source.mVertices = { { 1.0f, 2.0f, 3.0f }, { 4.0f, 5.0f, 6.0f }, { 7.0f, 8.0f, 9.0f } };
    source.mNormals = { { 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    source.mUVList.resize(1);
    source.mUVList[0] = { { 0.1f, 0.2f }, { 0.3f, 0.4f }, { 0.5f, 0.6f } };
    source.mColors = { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
    source.mTriangles = { 0, 1, 2 };

    const Render::MeshData mesh = Nif::convertMesh(source);
    if (mesh.vertices.size() != 3 || mesh.indices.size() != 3)
        throw std::runtime_error("NIF mesh conversion returned the wrong size");
    expectNear(mesh.vertices[1].position[2], 6.0f, "position");
    expectNear(mesh.vertices[2].texcoord[0], 0.5f, "texcoord");
    expectNear(mesh.vertices[0].color[0], 1.0f, "color");

    source.mNormals.clear();
    source.mUVList.clear();
    source.mColors.clear();
    const Render::MeshData defaults = Nif::convertMesh(source);
    expectNear(defaults.vertices[0].normal[2], 1.0f, "default normal");
    expectNear(defaults.vertices[0].color[3], 1.0f, "default color");

    source.mTriangles = { 0, 1, 3 };
    bool rejectedInvalidIndex = false;
    try
    {
        Nif::convertMesh(source);
    }
    catch (const std::runtime_error&)
    {
        rejectedInvalidIndex = true;
    }
    if (!rejectedInvalidIndex)
        throw std::runtime_error("invalid NIF index was accepted");

    Nif::NiTriStripsData strips;
    strips.mVertices = source.mVertices;
    strips.mStrips = { { 0, 1, 2 } };
    const Render::MeshData stripMesh = Nif::convertMesh(strips);
    if (stripMesh.indices != std::vector<uint32_t>({ 0, 1, 2 }))
        throw std::runtime_error("NIF triangle strip conversion returned the wrong winding");

    Nif::NiTriShapeData treeData;
    treeData.mVertices = source.mVertices;
    treeData.mTriangles = { 0, 1, 2 };
    Nif::NiTriShape shape;
    shape.mTransform = Nif::NiTransform::getIdentity();
    shape.mTransform.mTranslation.x() = 2.0f;
    shape.mData = &treeData;
    Nif::NiNode root;
    root.mTransform = Nif::NiTransform::getIdentity();
    root.mTransform.mTranslation.x() = 10.0f;
    root.mChildren.push_back(&shape);
    Nif::NIFFile file(VFS::Path::Normalized("synthetic.nif"));
    file.mRoots.push_back(&root);

    const std::vector<Render::MeshData> meshes = Nif::collectMeshes(Nif::FileView(file));
    if (meshes.size() != 1 || meshes.front().indices.size() != 3)
        throw std::runtime_error("NIF scene traversal did not collect the mesh");

    const std::vector<Render::MeshInstance> instances = Nif::collectMeshInstances(Nif::FileView(file));
    if (instances.size() != 1 || instances.front().mesh.indices.size() != 3)
        throw std::runtime_error("NIF scene traversal did not collect a mesh instance");
    expectNear(instances.front().transform.data[12], 12.0f, "composed mesh translation");

    std::cout << "NIF mesh conversion tests passed\n";
}
