#include <cmath>
#include <iostream>
#include <stdexcept>

#include <components/nif/data.hpp>
#include <components/nif/meshconverter.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/render/texture.hpp>
#include <components/resource/nifmeshmanager.hpp>

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
    Render::TextureData invalidTexture;
    if (invalidTexture.valid())
        throw std::runtime_error("empty neutral texture was marked valid");
    invalidTexture.width = 1;
    invalidTexture.height = 1;
    invalidTexture.pixels = { 255, 255, 255, 255 };
    if (!invalidTexture.valid())
        throw std::runtime_error("valid neutral texture was rejected");

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
    expectNear(mesh.vertices[0].tangent[0], 1.0f, "fallback tangent");
    expectNear(mesh.vertices[0].tangent[3], 1.0f, "tangent handedness");

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
    Nif::NiSourceTexture texture;
    texture.mFile = "textures\\synthetic.dds";
    Nif::NiSourceTexture normalTexture;
    normalTexture.mFile = "textures\\synthetic_n.dds";
    Nif::NiTexturingProperty texturing;
    texturing.mTextures.resize(Nif::NiTexturingProperty::BumpTexture + 1);
    texturing.mTextures.front().mEnabled = true;
    texturing.mTextures.front().mSourceTexture = &texture;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mSourceTexture = &normalTexture;
    Nif::NiMaterialProperty material;
    material.mDiffuse = { 0.25f, 0.5f, 0.75f };
    material.mAlpha = 0.75f;
    material.mEmissive = { 0.1f, 0.2f, 0.3f };
    material.mEmissiveMult = 2.f;
    material.mGlossiness = 42.f;
    auto lighting = std::make_unique<Nif::BSLightingShaderProperty>();
    lighting->mShaderFlags2 = Nif::BSLSFlag2_DoubleSided;
    lighting->mAlpha = 0.75f;
    lighting->mEmissive = { 0.1f, 0.2f, 0.3f };
    lighting->mEmissiveMult = 2.f;
    lighting->mGlossiness = 42.f;
    lighting->mTextureSet = Nif::BSShaderTextureSetPtr(nullptr);
    Nif::NiAlphaProperty alpha;
    alpha.mFlags = Nif::NiAlphaProperty::Flag_Blending | Nif::NiAlphaProperty::Flag_Testing;
    alpha.mThreshold = 128;
    Nif::NiTriShape shape;
    shape.mTransform = Nif::NiTransform::getIdentity();
    shape.mTransform.mTranslation.x() = 2.0f;
    shape.mData = &treeData;
    shape.mShaderProperty = lighting.get();
    shape.mProperties.push_back(&texturing);
    shape.mProperties.push_back(&material);
    shape.mAlphaProperty = &alpha;
    Nif::NiNode root;
    root.mTransform = Nif::NiTransform::getIdentity();
    root.mTransform.mTranslation.x() = 10.0f;
    root.mChildren.push_back(&shape);
    auto file = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("synthetic.nif"));
    file->mRoots.push_back(&root);

    const std::vector<Render::MeshInstance> instances = Nif::collectMeshInstances(Nif::FileView(*file));
    if (instances.size() != 1 || instances.front().mesh.indices.size() != 3)
        throw std::runtime_error("NIF scene traversal did not collect a mesh instance");
    expectNear(instances.front().transform.data[12], 12.0f, "composed mesh translation");
    if (instances.front().mesh.material.albedoTexture != "textures/synthetic.dds"
        || instances.front().mesh.material.normalTexture != "textures/synthetic_n.dds"
        || !instances.front().mesh.material.normalMap
        || !instances.front().mesh.material.alphaBlend || !instances.front().mesh.material.alphaTest
        || instances.front().mesh.material.alphaTestThreshold != 128
        || !instances.front().mesh.material.doubleSided)
        throw std::runtime_error("NIF material conversion lost texture or alpha state");
    expectNear(instances.front().mesh.material.diffuse.x, 0.25f, "material diffuse red");
    expectNear(instances.front().mesh.material.diffuse.w, 0.75f, "material alpha");
    expectNear(instances.front().mesh.material.emissive.z, 0.6f, "material emissive");
    expectNear(instances.front().mesh.material.glossiness, 42.f, "material glossiness");

    Resource::NifMeshManager meshManager(nullptr);
    const auto cached = meshManager.get(file);
    const auto cachedAgain = meshManager.get(file);
    if (cached != cachedAgain || cached->size() != 1 || cached->front().mesh.indices.size() != 3)
        throw std::runtime_error("NIF mesh manager did not reuse the converted mesh");

    const Render::MeshBatch batch = Render::batchMeshes({ instances.front(), instances.front() });
    if (batch.vertices.size() != 6 || batch.indices != std::vector<uint32_t>({ 0, 1, 2, 3, 4, 5 })
        || batch.draws.size() != 2 || batch.draws[1].firstIndex != 3 || batch.draws[1].vertexOffset != 3)
        throw std::runtime_error("renderer-neutral mesh batching returned the wrong layout");
    if (batch.draws.front().material.albedoTexture != "textures/synthetic.dds")
        throw std::runtime_error("renderer-neutral mesh batching dropped material data");
    expectNear(batch.vertices.front().color[0], 0.25f, "batched material diffuse red");
    expectNear(batch.vertices.front().color[3], 0.75f, "batched material alpha");
    expectNear(batch.vertices.front().material[0], 1.f - 42.f / 128.f, "batched material roughness");
    expectNear(batch.vertices.front().material[3], 0.6f, "batched material emission");
    expectNear(batch.draws[1].transform.data[12], 12.0f, "batched mesh translation");

    Render::WorldObject object{ 1, "synthetic.nif", {} };
    object.transform.position.x = 5.0f;
    const Render::MeshInstance transformed = Render::transformMeshInstance(object, instances.front());
    expectNear(transformed.transform.data[12], 17.0f, "cell object translation");

    Render::CellScene scene;
    scene.objects.push_back({ 2, "hidden.nif", {}, false });
    scene.objects.push_back({ 3, "synthetic.nif", {} });
    const std::vector<Render::MeshInstance> visibleMeshes = Render::collectCellMeshes(
        scene, [&](std::string_view model) -> const Resource::NifMeshManager::Meshes& {
            if (model != "synthetic.nif")
                throw std::runtime_error("cell mesh collection resolved a hidden object");
            return *cached;
        });
    if (visibleMeshes.size() != 1)
        throw std::runtime_error("cell mesh collection did not filter hidden objects");

    int worldObjectHandle = 0;
    int worldCellHandle = 0;
    int foreignObjectHandle = 0;
    int foreignCellHandle = 0;
    Render::WorldScene world;
    world.recordObject(
        &worldObjectHandle, &worldCellHandle, true, 0, 0, "world", "synthetic.nif", object.transform, true, "world-a");
    world.recordObject(&foreignObjectHandle, &foreignCellHandle, true, 1, 0, "foreign", "synthetic.nif",
        object.transform, true, "world-b");
    const std::vector<Render::MeshInstance> worldMeshes = Render::collectWorldMeshes(
        world, [&](std::string_view model) -> const Resource::NifMeshManager::Meshes& {
            if (model != "synthetic.nif")
                throw std::runtime_error("world mesh collection resolved an unexpected model");
            return *cached;
        }, "world-a");
    if (worldMeshes.size() != 1 || world.cellsInOrder("world-a").size() != 1
        || world.cellsInOrder("world-b").size() != 1)
        throw std::runtime_error("world mesh collection did not filter loaded worldspaces");

    Render::MeshInstance invalidBatch = instances.front();
    invalidBatch.mesh.indices = { 3 };
    bool rejectedInvalidBatchIndex = false;
    try
    {
        Render::batchMeshes({ invalidBatch });
    }
    catch (const std::runtime_error&)
    {
        rejectedInvalidBatchIndex = true;
    }
    if (!rejectedInvalidBatchIndex)
        throw std::runtime_error("renderer-neutral mesh batching accepted an invalid index");

    Render::MeshDraw opaqueDraw = {};
    Render::MeshDraw terrainDraw = {};
    Render::MeshDraw farTransparentDraw = {};
    Render::MeshDraw nearTransparentDraw = {};
    terrainDraw.material.terrainBlend = true;
    terrainDraw.material.terrainFirstLayer = true;
    farTransparentDraw.material.alphaBlend = true;
    farTransparentDraw.transform.data[12] = 100.f;
    nearTransparentDraw.material.alphaBlend = true;
    nearTransparentDraw.transform.data[12] = 2.f;
    const std::vector<std::size_t> drawOrder = Render::orderMeshDraws(
        { opaqueDraw, terrainDraw, nearTransparentDraw, farTransparentDraw }, { 0.f, 0.f, 0.f });
    if (drawOrder != std::vector<std::size_t>({ 0, 1, 3, 2 }))
        throw std::runtime_error("renderer-neutral mesh draw ordering was not deterministic");

    std::cout << "NIF mesh conversion tests passed\n";
}
