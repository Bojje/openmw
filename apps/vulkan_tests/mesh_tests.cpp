#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <components/nif/data.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/extra.hpp>
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
    if (Render::makeSpecularTexturePath("textures/stone.dds", "_spec") != "textures/stone_spec.dds"
        || !Render::makeSpecularTexturePath("textures/stone", "_spec").empty()
        || !Render::makeSpecularTexturePath("textures/stone.dds", "").empty())
        throw std::runtime_error("specular texture pattern mapping is not deterministic");

    Render::TextureData invalidTexture;
    if (invalidTexture.valid())
        throw std::runtime_error("empty neutral texture was marked valid");
    invalidTexture.width = 1;
    invalidTexture.height = 1;
    invalidTexture.pixels = { 255, 255, 255, 255 };
    if (!invalidTexture.valid())
        throw std::runtime_error("valid neutral texture was rejected");

    auto skinning = std::make_shared<Render::SkinningData>();
    skinning->vertices.resize(1);
    skinning->vertices.front().weights[0] = 1.f;
    skinning->boneNames.push_back("Root Bone");
    Render::Mat4 identity = {};
    identity.data[0] = identity.data[5] = identity.data[10] = identity.data[15] = 1.f;
    skinning->inverseBindMatrices.push_back(identity);
    if (!skinning->valid(1))
        throw std::runtime_error("valid neutral skinning data was rejected");
    skinning->vertices.front().weights[0] = 0.f;
    if (skinning->valid(1))
        throw std::runtime_error("neutral skinning data without an influence was accepted");

    skinning->vertices.front().weights[0] = 1.f;
    Render::MeshData skinnedMesh;
    skinnedMesh.vertices.push_back({ { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, {}, {}, {}, {}, {} });
    skinnedMesh.skinning = std::make_shared<const Render::SkinningData>(*skinning);
    Render::Mat4 bone = identity;
    bone.data[12] = 2.f;
    const Render::MeshData posedMesh = Render::skinMesh(skinnedMesh, std::span(&bone, 1));
    if (posedMesh.skinning || posedMesh.vertices.front().position[0] != 3.f
        || posedMesh.vertices.front().position[1] != 0.f || posedMesh.vertices.front().normal[2] != 1.f)
        throw std::runtime_error("neutral CPU skinning did not apply the bone transform");

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
    Nif::NiSourceTexture glowTexture;
    glowTexture.mFile = "textures\\synthetic_glow.dds";
    Nif::NiTexturingProperty texturing;
    texturing.mTextures.resize(Nif::NiTexturingProperty::BumpTexture + 1);
    texturing.mTextures.front().mEnabled = true;
    texturing.mTextures.front().mSourceTexture = &texture;
    texturing.mTextures.front().mClamp = 0;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mSourceTexture = &normalTexture;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mClamp = 1;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mSourceTexture = &glowTexture;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mClamp = 2;
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
    shape.mController = Nif::NiTimeControllerPtr(nullptr);
    shape.mData = &treeData;
    shape.mShaderProperty = lighting.get();
    shape.mProperties.push_back(&texturing);
    shape.mProperties.push_back(&material);
    shape.mAlphaProperty = &alpha;
    Nif::NiSkinData skinData;
    skinData.mBones.resize(1);
    skinData.mBones.front().mTransform = Nif::NiTransform::getIdentity();
    skinData.mBones.front().mWeights = { { 0, 1.f }, { 1, 1.f }, { 2, 1.f } };
    Nif::NiSkinInstance skin;
    skin.mData = &skinData;
    skin.mBones.resize(1);
    Nif::NiNode skinBone;
    skinBone.mName = "Root Bone";
    skin.mBones.front() = &skinBone;
    shape.mSkin = &skin;
    Nif::NiNode root;
    root.mTransform = Nif::NiTransform::getIdentity();
    root.mTransform.mTranslation.x() = 10.0f;
    root.mController = Nif::NiTimeControllerPtr(nullptr);

    auto animatedData = std::make_unique<Nif::NiKeyframeData>();
    animatedData->mTranslations = std::make_shared<Nif::Vector3KeyMap>();
    animatedData->mTranslations->mInterpolationType = Nif::InterpolationType_Quadratic;
    Nif::KeyT<osg::Vec3f> firstTranslation{};
    firstTranslation.mValue = osg::Vec3f(0.f, 0.f, 0.f);
    Nif::KeyT<osg::Vec3f> secondTranslation{};
    secondTranslation.mValue = osg::Vec3f(4.f, 0.f, 0.f);
    animatedData->mTranslations->mKeys.emplace_back(0.f, firstTranslation);
    animatedData->mTranslations->mKeys.emplace_back(1.f, secondTranslation);
    auto animatedController = std::make_unique<Nif::NiKeyframeController>();
    animatedController->mFlags = Nif::NiTimeController::Flag_Active;
    animatedController->mFrequency = 1.f;
    animatedController->mPhase = 0.f;
    animatedController->mTimeStart = 0.f;
    animatedController->mTimeStop = 1.f;
    animatedController->mInterpolator = Nif::NiInterpolatorPtr(nullptr);
    animatedController->mData = animatedData.get();
    Nif::NiNode animatedBone;
    animatedBone.mName = "Root Bone";
    animatedBone.mController = animatedController.get();
    root.mChildren.push_back(&animatedBone);

    root.mChildren.push_back(&shape);
    auto file = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("synthetic.nif"));
    file->mUseSkinning = true;
    file->mRoots.push_back(&root);

    const std::vector<Render::MeshInstance> instances = Nif::collectMeshInstances(Nif::FileView(*file));
    if (instances.size() != 1 || instances.front().mesh.indices.size() != 3)
        throw std::runtime_error("NIF scene traversal did not collect a mesh instance");
    expectNear(instances.front().transform.data[12], 12.0f, "composed mesh translation");
    if (instances.front().mesh.material.albedoTexture != "textures/synthetic.dds"
        || instances.front().mesh.material.normalTexture != "textures/synthetic_n.dds"
        || instances.front().mesh.material.emissiveTexture != "textures/synthetic_glow.dds"
        || !instances.front().mesh.material.normalMap
        || !instances.front().mesh.material.alphaBlend || !instances.front().mesh.material.alphaTest
        || instances.front().mesh.material.alphaTestThreshold != 128
        || !instances.front().mesh.material.doubleSided
        || instances.front().mesh.material.albedoWrapU || instances.front().mesh.material.albedoWrapV
        || instances.front().mesh.material.normalWrapU || !instances.front().mesh.material.normalWrapV)
        throw std::runtime_error("NIF material conversion lost texture or alpha state");
    if (!instances.front().mesh.skinning || !instances.front().mesh.skinning->valid(3)
        || instances.front().mesh.skinning->vertices[1].weights[0] != 1.f
        || instances.front().mesh.skinning->boneNames != std::vector<std::string>{ "Root Bone" })
        throw std::runtime_error("NIF skinning metadata was not preserved at the neutral boundary");
    if (!instances.front().mesh.material.emissiveWrapU || instances.front().mesh.material.emissiveWrapV)
        throw std::runtime_error("NIF material conversion lost emissive texture wrapping");

    const std::array<std::string, 1> animatedBoneNames{ "Root Bone" };
    const std::vector<Render::Mat4> animatedPose
        = Nif::collectBonePose(Nif::FileView(*file), animatedBoneNames, 0.5f);
    if (animatedPose.size() != 1)
        throw std::runtime_error("NIF pose sampler did not find the requested bone");
    expectNear(animatedPose.front().data[12], 12.f, "sampled bone translation");
    const std::vector<Render::Mat4> earlyAnimatedPose
        = Nif::collectBonePose(Nif::FileView(*file), animatedBoneNames, 0.25f);
    expectNear(earlyAnimatedPose.front().data[12], 10.625f, "quadratic sampled bone translation");

    Nif::NiTextKeyExtraData sequenceTextKeys;
    sequenceTextKeys.mRecordType = Nif::RC_NiTextKeyExtraData;
    Nif::NiStringExtraData sequenceBoneName;
    sequenceBoneName.mRecordType = Nif::RC_NiStringExtraData;
    sequenceBoneName.mData = "Root Bone";
    auto sequenceController = std::make_unique<Nif::NiKeyframeController>();
    sequenceController->mRecordType = Nif::RC_NiKeyframeController;
    sequenceController->mFlags = Nif::NiTimeController::Flag_Active;
    sequenceController->mFrequency = 1.f;
    sequenceController->mPhase = 0.f;
    sequenceController->mTimeStart = 0.f;
    sequenceController->mTimeStop = 1.f;
    sequenceController->mInterpolator = Nif::NiInterpolatorPtr(nullptr);
    sequenceController->mNext = Nif::NiTimeControllerPtr(nullptr);
    sequenceController->mData = animatedData.get();
    Nif::NiSequenceStreamHelper sequence;
    sequence.mExtra = Nif::ExtraPtr(nullptr);
    sequence.mController = sequenceController.get();
    sequence.mExtraList = { &sequenceTextKeys, &sequenceBoneName };
    auto kfFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("synthetic.kf"));
    kfFile->mRoots.push_back(&sequence);
    const std::vector<Render::Mat4> externalPose
        = Nif::collectBonePose(Nif::FileView(*kfFile), animatedBoneNames, 0.25f);
    if (externalPose.size() != 1)
        throw std::runtime_error("neutral KF pose sampler did not find the controller bone");
    expectNear(externalPose.front().data[12], 0.625f, "sampled external KF translation");

    expectNear(instances.front().mesh.material.diffuse.x, 0.25f, "material diffuse red");
    expectNear(instances.front().mesh.material.diffuse.w, 0.75f, "material alpha");
    expectNear(instances.front().mesh.material.emissive.z, 0.6f, "material emissive");
    expectNear(instances.front().mesh.material.glossiness, 42.f, "material glossiness");

    Nif::BSEffectShaderProperty effect;
    effect.mSourceTexture = "textures\\effect.dds";
    effect.mNormalTexture = "textures\\effect_n.dds";
    effect.mClamp = 1;
    effect.mBaseColor = { 0.25f, 0.5f, 0.75f, 0.5f };
    effect.mBaseColorScale = 2.f;
    effect.mEmittanceColor = { 0.1f, 0.2f, 0.3f };
    effect.mShaderFlags2 = Nif::BSLSFlag2_DoubleSided;
    Nif::NiTriShape effectShape;
    effectShape.mData = &treeData;
    effectShape.mShaderProperty = &effect;
    effectShape.mAlphaProperty = nullptr;
    auto effectFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("effect.nif"));
    effectFile->mRoots.push_back(&effectShape);
    const std::vector<Render::MeshInstance> effectInstances
        = Nif::collectMeshInstances(Nif::FileView(*effectFile));
    if (effectInstances.size() != 1 || effectInstances.front().mesh.material.albedoTexture != "textures/effect.dds"
        || effectInstances.front().mesh.material.normalTexture != "textures/effect_n.dds"
        || !effectInstances.front().mesh.material.alphaBlend || !effectInstances.front().mesh.material.doubleSided
        || effectInstances.front().mesh.material.albedoWrapU || !effectInstances.front().mesh.material.albedoWrapV)
        throw std::runtime_error("NIF effect shader material conversion lost neutral state");
    expectNear(effectInstances.front().mesh.material.diffuse.x, 0.5f, "effect diffuse red");
    expectNear(effectInstances.front().mesh.material.diffuse.w, 0.5f, "effect alpha");
    expectNear(effectInstances.front().mesh.material.emissive.z, 0.3f, "effect emissive blue");

    auto shaderTextureSet = std::make_unique<Nif::BSShaderTextureSet>();
    shaderTextureSet->mTextures = { "textures\\shader.dds", "textures\\shader_n.dds",
        "textures\\shader_glow.dds" };
    auto shaderLighting = std::make_unique<Nif::BSLightingShaderProperty>();
    shaderLighting->mTextureSet = Nif::BSShaderTextureSetPtr(shaderTextureSet.get());
    shaderLighting->mClamp = 1;
    Nif::NiTriShape shaderShape;
    shaderShape.mData = &treeData;
    shaderShape.mShaderProperty = shaderLighting.get();
    shaderShape.mAlphaProperty = nullptr;
    auto shaderFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("shader.nif"));
    shaderFile->mRoots.push_back(&shaderShape);
    const std::vector<Render::MeshInstance> shaderInstances
        = Nif::collectMeshInstances(Nif::FileView(*shaderFile));
    if (shaderInstances.size() != 1 || shaderInstances.front().mesh.material.albedoTexture != "textures/shader.dds"
        || shaderInstances.front().mesh.material.normalTexture != "textures/shader_n.dds"
        || shaderInstances.front().mesh.material.emissiveTexture != "textures/shader_glow.dds"
        || shaderInstances.front().mesh.material.albedoWrapU || !shaderInstances.front().mesh.material.albedoWrapV
        || shaderInstances.front().mesh.material.normalWrapU || !shaderInstances.front().mesh.material.normalWrapV
        || shaderInstances.front().mesh.material.emissiveWrapU
        || !shaderInstances.front().mesh.material.emissiveWrapV)
        throw std::runtime_error("BS shader texture-set conversion lost emissive texture state");

    Nif::BSShaderNoLightingProperty noLighting;
    noLighting.mFilename = "textures\\unlit.dds";
    noLighting.mClamp = 1;
    Nif::NiTriShape noLightingShape;
    noLightingShape.mData = &treeData;
    noLightingShape.mShaderProperty = &noLighting;
    noLightingShape.mAlphaProperty = nullptr;
    auto noLightingFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("unlit.nif"));
    noLightingFile->mRoots.push_back(&noLightingShape);
    const std::vector<Render::MeshInstance> noLightingInstances
        = Nif::collectMeshInstances(Nif::FileView(*noLightingFile));
    if (noLightingInstances.size() != 1
        || noLightingInstances.front().mesh.material.albedoTexture != "textures/unlit.dds"
        || noLightingInstances.front().mesh.material.albedoWrapU
        || !noLightingInstances.front().mesh.material.albedoWrapV)
        throw std::runtime_error("NIF no-lighting shader material conversion lost neutral state");

    Resource::NifMeshManager meshManager(nullptr);
    auto controller = std::make_unique<Nif::NiTimeController>();
    controller->mTimeStart = 1.f;
    controller->mTimeStop = 4.f;
    file->mRecords.push_back(std::move(controller));
    const std::optional<float> animationDuration = meshManager.getAnimationDuration(file);
    if (!animationDuration || *animationDuration != 3.f)
        throw std::runtime_error("NIF mesh manager did not expose neutral controller duration");
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

    Render::WorldObject object{ 1, "synthetic.nif", {}, true, false, {}, {} };
    object.transform.position.x = 5.0f;
    const Render::MeshInstance transformed = Render::transformMeshInstance(object, instances.front());
    expectNear(transformed.transform.data[12], 17.0f, "cell object translation");

    Render::CellScene scene;
    scene.objects.push_back({ 2, "hidden.nif", {}, false, false, {}, {} });
    scene.objects.push_back({ 3, "synthetic.nif", {}, true, false, {}, {} });
    scene.objects.push_back({ 4, "animated.nif", {}, true, true, {}, {} });
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
