#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include <components/nif/data.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/meshconverter.hpp>
#include <components/nif/particle.hpp>
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
    skinnedMesh.vertices.push_back({ { 1.f, 0.f, 0.f }, { 0.f, 0.f, 1.f }, {}, {}, {}, {}, {}, {}, {}, {}, {} });
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

    Nif::NiParticlesData particleSource;
    particleSource.mActiveCount = 2;
    particleSource.mVertices = { { 1.f, 2.f, 3.f }, { -2.f, 1.f, 0.f } };
    particleSource.mRadii = { 2.f };
    particleSource.mSizes = { 1.f, 2.f };
    particleSource.mColors = { { 0.25f, 0.5f, 0.75f, 0.8f }, { 0.9f, 0.8f, 0.7f, 0.6f } };
    particleSource.mRotationAngles = { 3.14159265358979323846f / 2.f, 0.f };
    particleSource.mRotationAxes = { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f } };
    const Render::MeshData particles = Nif::convertParticles(particleSource);
    if (particles.vertices.size() != 8 || particles.indices.size() != 12
        || std::abs(particles.vertices[0].position[0] - 2.f) > 1e-5f
        || std::abs(particles.vertices[0].position[1] + 2.f) > 1e-5f
        || particles.vertices[4].position[0] != -4.f
        || std::abs(particles.vertices[0].color[0] - 0.25f) > 1e-5f
        || std::abs(particles.vertices[0].color[3] - 0.8f) > 1e-5f
        || std::abs(particles.vertices[4].color[2] - 0.7f) > 1e-5f
        || particles.vertices[0].tangent[0] != 1.f || particles.vertices[4].tangent[1] != 1.f
        || particles.vertices[7].texcoord[1] != 1.f)
        throw std::runtime_error("NIF particle conversion did not create deterministic quad snapshots");

    Nif::NiPSysData simulatedParticleSource;
    simulatedParticleSource.mActiveCount = 1;
    simulatedParticleSource.mVertices = { { 1.f, 2.f, 3.f } };
    simulatedParticleSource.mRadii = { 1.f };
    simulatedParticleSource.mParticles.resize(1);
    simulatedParticleSource.mParticles.front().mVelocity = { 2.f, 0.f, 0.f };
    simulatedParticleSource.mParticles.front().mAge = 0.f;
    simulatedParticleSource.mParticles.front().mLifespan = 0.5f;
    simulatedParticleSource.mRotationSpeeds = { 2.f * 3.14159265358979323846f };
    Nif::NiPSysGravityModifier gravity;
    gravity.mActive = true;
    gravity.mForceType = Nif::ForceType::Wind;
    gravity.mGravityAxis = { 0.f, 0.f, -1.f };
    gravity.mStrength = 4.f;
    Nif::NiPSysGrowFadeModifier growFade;
    growFade.mActive = true;
    growFade.mGrowTime = 0.5f;
    growFade.mFadeTime = 0.25f;
    growFade.mBaseScale = 1.f;
    Nif::NiParticleSystem particleSystem;
    particleSystem.mController = Nif::NiTimeControllerPtr(nullptr);
    particleSystem.mModifiers = { Nif::NiPSysModifierPtr(&gravity), Nif::NiPSysModifierPtr(&growFade) };
    const Render::MeshData simulatedParticles = Nif::convertParticles(simulatedParticleSource);
    const Render::MeshData convertedModifiers = Nif::convertParticles(simulatedParticleSource, &particleSystem);
    if (!simulatedParticles.particles || simulatedParticles.particles->states.size() != 1)
        throw std::runtime_error("NIF particle conversion lost neutral particle state");
    if (!convertedModifiers.particles || !convertedModifiers.particles->simulation
        || std::abs(convertedModifiers.particles->simulation->acceleration.z + 4.f) > 1e-5f
        || convertedModifiers.particles->simulation->growTime != 0.5f)
        throw std::runtime_error("NIF particle conversion lost neutral modifier data");
    const Render::MeshData advancedParticles = Render::advanceParticleMesh(simulatedParticles, 0.25f);
    if (advancedParticles.particles || std::abs(advancedParticles.vertices[0].tangent[0] - 1.5f) > 1e-5f
        || std::abs(advancedParticles.vertices[0].position[0] - 1.f) > 1e-5f
        || std::abs(advancedParticles.vertices[0].position[1] + 1.f) > 1e-5f)
        throw std::runtime_error("renderer-neutral particle state did not advance position and rotation");
    const Render::MeshData expiredParticles = Render::advanceParticleMesh(simulatedParticles, 1.f);
    if (expiredParticles.vertices[0].color[3] != 0.f)
        throw std::runtime_error("renderer-neutral particle state did not expire a dead particle");

    Render::MeshData modifiedParticles = simulatedParticles;
    auto simulation = std::make_shared<Render::ParticleSimulationData>();
    simulation->acceleration = { 0.f, 0.f, -4.f };
    simulation->growTime = 0.5f;
    simulation->rotationSpeed = 1.f;
    auto modifiedParticleData = std::make_shared<Render::ParticleMeshData>(*simulatedParticles.particles);
    modifiedParticleData->simulation = std::move(simulation);
    modifiedParticles.particles = std::move(modifiedParticleData);
    const Render::MeshData advancedModifiers = Render::advanceParticleMesh(modifiedParticles, 0.25f);
    if (std::abs(std::hypot(advancedModifiers.vertices[0].position[0], advancedModifiers.vertices[0].position[1])
                   - std::sqrt(2.f) * 0.5f)
            > 1e-5f
        || std::abs(advancedModifiers.vertices[0].tangent[2] - 2.875f) > 1e-5f)
        throw std::runtime_error("renderer-neutral particle modifiers did not affect scale and acceleration");

    Nif::NiPSysData collisionParticleSource;
    collisionParticleSource.mActiveCount = 1;
    collisionParticleSource.mVertices = { { 0.f, 0.f, 1.f } };
    collisionParticleSource.mRadii = { 1.f };
    collisionParticleSource.mParticles.resize(1);
    collisionParticleSource.mParticles.front().mVelocity = { 0.f, 0.f, -4.f };
    collisionParticleSource.mParticles.front().mLifespan = 2.f;
    Nif::NiPlanarCollider planarCollider;
    planarCollider.mRecordType = Nif::RC_NiPlanarCollider;
    planarCollider.mBounceFactor = 0.5f;
    planarCollider.mDieOnCollision = true;
    planarCollider.mSpawnOnCollision = true;
    planarCollider.mExtents = { 10.f, 10.f };
    planarCollider.mPosition = { 0.f, 0.f, 0.f };
    planarCollider.mXVector = { 1.f, 0.f, 0.f };
    planarCollider.mYVector = { 0.f, 1.f, 0.f };
    planarCollider.mPlaneNormal = { 0.f, 0.f, 1.f };
    planarCollider.mPlaneDistance = 0.f;
    Nif::NiPlanarCollider secondPlanarCollider;
    secondPlanarCollider.mRecordType = Nif::RC_NiPlanarCollider;
    secondPlanarCollider.mBounceFactor = 0.25f;
    secondPlanarCollider.mDieOnCollision = false;
    secondPlanarCollider.mSpawnOnCollision = false;
    secondPlanarCollider.mExtents = { 1.f, 1.f };
    secondPlanarCollider.mPosition = { 100.f, 100.f, 0.f };
    secondPlanarCollider.mXVector = { 1.f, 0.f, 0.f };
    secondPlanarCollider.mYVector = { 0.f, 1.f, 0.f };
    secondPlanarCollider.mPlaneNormal = { 0.f, 0.f, 1.f };
    secondPlanarCollider.mPlaneDistance = 0.f;
    secondPlanarCollider.mNext = Nif::NiParticleModifierPtr(nullptr);
    planarCollider.mNext = Nif::NiParticleModifierPtr(&secondPlanarCollider);
    Nif::NiParticleSystemController collisionController;
    collisionController.mRecordType = Nif::RC_NiParticleSystemController;
    collisionController.mFlags = Nif::NiTimeController::Flag_Active;
    collisionController.mParticles.resize(1);
    collisionController.mParticles.front().mCode = 0;
    collisionController.mParticles.front().mVelocity = { 0.f, 0.f, -4.f };
    collisionController.mParticles.front().mLifespan = 2.f;
    collisionController.mCollider = Nif::NiParticleModifierPtr(&planarCollider);
    collisionController.mNext = Nif::NiTimeControllerPtr(nullptr);
    Nif::NiParticleSystem collisionParticleSystem;
    collisionParticleSystem.mController = Nif::NiTimeControllerPtr(&collisionController);
    const Render::MeshData collisionParticles
        = Nif::convertParticles(collisionParticleSource, &collisionParticleSystem);
    const Render::MeshData bouncedParticles = Render::advanceParticleMesh(collisionParticles, 0.5f);
    if (!collisionParticles.particles || !collisionParticles.particles->simulation
        || collisionParticles.particles->simulation->colliders.size() != 2 || bouncedParticles.vertices.size() != 8
        || std::abs(bouncedParticles.vertices[0].tangent[2] - 0.5f) > 1e-5f
        || bouncedParticles.vertices[0].color[3] != 0.f)
        throw std::runtime_error("neutral particle planar collider did not reflect motion");

    Nif::NiPSysSpawnModifier spawnModifier;
    spawnModifier.mActive = true;
    spawnModifier.mNumSpawnGenerations = 2;
    spawnModifier.mPercentageSpawned = 1.f;
    spawnModifier.mMinNumToSpawn = 1;
    spawnModifier.mMaxNumToSpawn = 1;
    spawnModifier.mSpawnSpeedVariation = 0.f;
    spawnModifier.mSpawnDirVariation = 0.f;
    spawnModifier.mLifespan = 1.f;
    spawnModifier.mLifespanVariation = 0.f;
    Nif::NiParticleSystem spawnParticleSystem;
    spawnParticleSystem.mController = Nif::NiTimeControllerPtr(nullptr);
    spawnParticleSystem.mModifiers = { Nif::NiPSysModifierPtr(&spawnModifier) };
    const Render::MeshData spawnParticles
        = Nif::convertParticles(simulatedParticleSource, &spawnParticleSystem);
    const Render::MeshData generatedParticles = Render::advanceParticleMesh(spawnParticles, 0.75f);
    if (!spawnParticles.particles || !spawnParticles.particles->simulation
        || !spawnParticles.particles->simulation->spawn || generatedParticles.vertices.size() != 8
        || generatedParticles.vertices[4].color[3] == 0.f
        || std::abs(generatedParticles.vertices[4].tangent[0] - 3.f) > 1e-5f)
        throw std::runtime_error("neutral particle spawn modifier did not generate a bounded child snapshot");
    const Render::MeshData multiGenerationParticles = Render::advanceParticleMesh(spawnParticles, 2.75f);
    if (multiGenerationParticles.vertices.size() != 12 || multiGenerationParticles.indices.size() != 18
        || multiGenerationParticles.vertices[8].color[3] != 0.f)
        throw std::runtime_error("neutral particle spawn modifier did not process bounded descendants");

    Nif::NiPSysData modernParticleSource;
    modernParticleSource.mActiveCount = 1;
    modernParticleSource.mVertices = { { 1.f, 0.f, 0.f }, { 4.f, 0.f, 0.f } };
    modernParticleSource.mRadii = { 1.f, 2.f };
    Nif::NiParticleSystemController modernController;
    modernController.mRecordType = Nif::RC_NiParticleSystemController;
    modernController.mFlags = Nif::NiTimeController::Flag_Active;
    modernController.mInitialSize = 1.5f;
    modernController.mInitialColor = { 0.2f, 0.4f, 0.6f, 0.8f };
    modernController.mEmitStartTime = 0.f;
    modernController.mEmitStopTime = 2.f;
    modernController.mEmitFlags = Nif::NiParticleSystemController::EmitFlag_NoAutoAdjust;
    modernController.mBirthRate = 4.f;
    modernController.mLifetime = 1.f;
    modernController.mSpeed = 2.f;
    modernController.mInitialNormal = { 1.f, 0.f, 0.f };
    modernController.mEmitterDimensions = { 2.f, 4.f, 6.f };
    modernController.mNumParticles = 4;
    modernController.mParticles.resize(1);
    modernController.mParticles.front().mCode = 1;
    modernController.mParticles.front().mVelocity = { 0.f, 0.f, 0.f };
    modernController.mParticles.front().mLifespan = 1.f;
    modernController.mCollider = Nif::NiParticleModifierPtr(nullptr);
    modernController.mNext = Nif::NiTimeControllerPtr(nullptr);
    Nif::NiParticleSystem modernParticleSystem;
    modernParticleSystem.mController = Nif::NiTimeControllerPtr(&modernController);
    modernParticleSource.mNumParticles = 4;
    const Render::MeshData modernParticles = Nif::convertParticles(modernParticleSource, &modernParticleSystem);
    if (modernParticles.vertices.empty() || std::abs(modernParticles.vertices.front().tangent[0] - 4.f) > 1e-5f
        || std::abs(modernParticles.vertices.front().position[0] + 3.f) > 1e-5f
        || std::abs(modernParticles.vertices.front().color[0] - 0.2f) > 1e-5f
        || std::abs(modernParticles.vertices.front().color[3] - 0.8f) > 1e-5f)
        throw std::runtime_error("modern NIF particle controller state did not map its particle code");
    const Render::MeshData emittedParticles = Render::advanceParticleMesh(modernParticles, 0.5f);
    if (emittedParticles.particles || emittedParticles.vertices.size() != 12 || emittedParticles.indices.size() != 18
        || emittedParticles.vertices[4].tangent[0] < -1.f || emittedParticles.vertices[4].tangent[0] > 1.f
        || emittedParticles.vertices[4].tangent[1] < -2.f || emittedParticles.vertices[4].tangent[1] > 2.f
        || emittedParticles.vertices[4].tangent[2] < -3.f || emittedParticles.vertices[4].tangent[2] > 3.f)
        throw std::runtime_error("neutral particle controller did not emit bounded deterministic snapshots");

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
    Nif::NiSourceTexture glossTexture;
    glossTexture.mFile = "textures\\synthetic_gloss.dds";
    Nif::NiTexturingProperty texturing;
    texturing.mTextures.resize(Nif::NiTexturingProperty::DecalTexture + 1);
    texturing.mTextures.front().mEnabled = true;
    texturing.mTextures.front().mSourceTexture = &texture;
    texturing.mTextures.front().mClamp = 0;
    texturing.mTextures.front().mHasTransform = true;
    texturing.mTextures.front().mTransform.mOffset = { 0.25f, 0.125f };
    texturing.mTextures.front().mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures.front().mTransform.mRotation = 0.f;
    texturing.mTextures.front().mTransform.mOrigin = { 0.f, 0.f };
    texturing.mTextures.front().mTransform.mTransformMethod = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mSourceTexture = &texture;
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mClamp = 1;
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mTransform.mOffset = { 0.5f, 0.25f };
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::DarkTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mSourceTexture = &normalTexture;
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mClamp = 2;
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mTransform.mOffset = { 0.1f, 0.2f };
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mTransform.mScale = { 2.f, 3.f };
    texturing.mTextures[Nif::NiTexturingProperty::DetailTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mSourceTexture = &normalTexture;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mClamp = 1;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mTransform.mOffset = { 0.3f, 0.6f };
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::BumpTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mEnvMapLumaBias = { 0.6f, 0.2f };
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mSourceTexture = &glowTexture;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mClamp = 2;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mTransform.mOffset = { 0.7f, 0.8f };
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::GlowTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mSourceTexture = &glossTexture;
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mClamp = 1;
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mTransform.mOffset = { 0.9f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::GlossTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mEnabled = true;
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mSourceTexture = &glowTexture;
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mClamp = 0;
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mHasTransform = true;
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mTransform.mOffset = { -0.1f, 0.4f };
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mTransform.mScale = { 1.f, 1.f };
    texturing.mTextures[Nif::NiTexturingProperty::DecalTexture].mTransform.mTransformMethod
        = Nif::NiTextureTransform::Method::MayaLegacy;
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
    lighting->mSpecular = { 0.2f, 0.4f, 0.8f };
    lighting->mGlossiness = 42.f;
    lighting->mSpecStrength = 0.65f;
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
    animatedController->mNext = Nif::NiTimeControllerPtr(nullptr);
    animatedController->mData = animatedData.get();
    auto stackedData = std::make_unique<Nif::NiKeyframeData>();
    stackedData->mTranslations = std::make_shared<Nif::Vector3KeyMap>();
    stackedData->mTranslations->mInterpolationType = Nif::InterpolationType_Constant;
    Nif::KeyT<osg::Vec3f> stackedTranslation{};
    stackedTranslation.mValue = osg::Vec3f(6.f, 0.f, 0.f);
    stackedData->mTranslations->mKeys.emplace_back(0.f, stackedTranslation);
    auto stackedController = std::make_unique<Nif::NiKeyframeController>();
    stackedController->mFlags = Nif::NiTimeController::Flag_Active;
    stackedController->mFrequency = 1.f;
    stackedController->mPhase = 0.f;
    stackedController->mTimeStart = 0.f;
    stackedController->mTimeStop = 1.f;
    stackedController->mInterpolator = Nif::NiInterpolatorPtr(nullptr);
    stackedController->mNext = Nif::NiTimeControllerPtr(nullptr);
    stackedController->mData = stackedData.get();
    animatedController->mNext = stackedController.get();
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
        || instances.front().mesh.material.darkTexture != "textures/synthetic.dds"
        || instances.front().mesh.material.detailTexture != "textures/synthetic_n.dds"
        || instances.front().mesh.material.decalTexture != "textures/synthetic_glow.dds"
        || instances.front().mesh.material.normalTexture != "textures/synthetic_n.dds"
        || instances.front().mesh.material.emissiveTexture != "textures/synthetic_glow.dds"
        || instances.front().mesh.material.specularTexture != "textures/synthetic_gloss.dds"
        || !instances.front().mesh.material.specularMaterial
        || std::abs(instances.front().mesh.material.specularStrength - 0.65f) > 1e-5f
        || std::abs(instances.front().mesh.material.specular.x - 0.2f) > 1e-5f
        || std::abs(instances.front().mesh.material.specular.y - 0.4f) > 1e-5f
        || std::abs(instances.front().mesh.material.specular.z - 0.8f) > 1e-5f
        || std::abs(instances.front().mesh.material.emissiveLumaBias[0] - 0.6f) > 1e-5f
        || std::abs(instances.front().mesh.material.emissiveLumaBias[1] - 0.2f) > 1e-5f
        || !instances.front().mesh.material.normalMap
        || !instances.front().mesh.material.alphaBlend || !instances.front().mesh.material.alphaTest
        || instances.front().mesh.material.alphaTestThreshold != 128
        || !instances.front().mesh.material.doubleSided
        || instances.front().mesh.material.albedoWrapU || instances.front().mesh.material.albedoWrapV
        || instances.front().mesh.material.darkWrapU || !instances.front().mesh.material.darkWrapV
        || !instances.front().mesh.material.detailWrapU || instances.front().mesh.material.detailWrapV
        || instances.front().mesh.material.decalWrapU || instances.front().mesh.material.decalWrapV
        || instances.front().mesh.material.normalWrapU || !instances.front().mesh.material.normalWrapV)
        throw std::runtime_error("NIF material conversion lost texture or alpha state");
    expectNear(instances.front().mesh.vertices.front().texcoord[0], 0.25f, "base texture U transform");
    expectNear(instances.front().mesh.vertices.front().texcoord[1], 0.125f, "base texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[0], 0.5f, "dark texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[1], 0.25f, "dark texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[2], 0.1f, "detail texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[3], 0.2f, "detail texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[4], -0.1f, "decal texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[5], 0.4f, "decal texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[6], 0.3f, "normal texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[7], 0.6f, "normal texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[8], 0.7f, "emissive texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[9], 0.8f, "emissive texture V transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[10], 0.9f, "specular texture U transform");
    expectNear(instances.front().mesh.vertices.front().textureLayerCoords[11], 1.f, "specular texture V transform");
    if (!instances.front().mesh.skinning || !instances.front().mesh.skinning->valid(3)
        || instances.front().mesh.skinning->vertices[1].weights[0] != 1.f
        || instances.front().mesh.skinning->boneNames != std::vector<std::string>{ "Root Bone" })
        throw std::runtime_error("NIF skinning metadata was not preserved at the neutral boundary");
    if (!instances.front().mesh.material.emissiveWrapU || instances.front().mesh.material.emissiveWrapV)
        throw std::runtime_error("NIF material conversion lost emissive texture wrapping");

    std::array<Render::Mat4, 2> sourceBones{ Render::identityMat4(), Render::identityMat4() };
    sourceBones[0].data[12] = 1.f;
    sourceBones[1].data[12] = 2.f;
    const std::array<std::string, 2> sourceBoneNames{ "Root Bone", "Arm Bone" };
    const std::array<std::string, 2> reorderedBoneNames{ "Arm Bone", "Root Bone" };
    const std::vector<Render::Mat4> reorderedBones
        = Render::remapBoneMatrices(sourceBones, sourceBoneNames, reorderedBoneNames);
    if (reorderedBones.size() != 2 || reorderedBones[0].data[12] != 2.f || reorderedBones[1].data[12] != 1.f)
        throw std::runtime_error("renderer-neutral skinning did not remap mismatched bone orders");
    const std::array<std::string, 2> mixedCaseBoneNames{ "arm bone", "root bone" };
    const std::vector<Render::Mat4> mixedCaseBones
        = Render::remapBoneMatrices(sourceBones, sourceBoneNames, mixedCaseBoneNames);
    if (mixedCaseBones.size() != 2 || mixedCaseBones[0].data[12] != 2.f || mixedCaseBones[1].data[12] != 1.f)
        throw std::runtime_error("renderer-neutral skinning did not match bone names case-insensitively");

    const std::array<std::string, 1> animatedBoneNames{ "Root Bone" };
    const std::vector<Render::Mat4> animatedPose
        = Nif::collectBonePose(Nif::FileView(*file), animatedBoneNames, 0.5f);
    if (animatedPose.size() != 1)
        throw std::runtime_error("NIF pose sampler did not find the requested bone");
    expectNear(animatedPose.front().data[12], 16.f, "stacked sampled bone translation");
    const std::vector<Render::Mat4> earlyAnimatedPose
        = Nif::collectBonePose(Nif::FileView(*file), animatedBoneNames, 0.25f);
    expectNear(earlyAnimatedPose.front().data[12], 16.f, "stacked sampled bone translation at early time");

    Nif::NiNode higherPriorityBone;
    higherPriorityBone.mName = "Root Bone";
    higherPriorityBone.mTransform = Nif::NiTransform::getIdentity();
    higherPriorityBone.mTransform.mTranslation.x() = 7.f;
    higherPriorityBone.mController = Nif::NiTimeControllerPtr(nullptr);
    auto priorityFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("priority.nif"));
    priorityFile->mRoots.push_back(&animatedBone);
    priorityFile->mRoots.push_back(&higherPriorityBone);
    const std::vector<Render::Mat4> priorityPose
        = Nif::collectBonePose(Nif::FileView(*priorityFile), animatedBoneNames, 0.5f);
    if (priorityPose.size() != 1)
        throw std::runtime_error("NIF priority pose sampler did not find the duplicate bone");
    expectNear(priorityPose.front().data[12], 7.f, "later animation source priority");

    Nif::NiTextKeyExtraData sequenceTextKeys;
    sequenceTextKeys.mRecordType = Nif::RC_NiTextKeyExtraData;
    sequenceTextKeys.mList.push_back({ 0.25f, "Idle: Start" });
    sequenceTextKeys.mList.push_back({ 0.5f, "Idle: Mid" });
    sequenceTextKeys.mList.push_back({ 0.75f, "Idle: Stop" });
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
    kfFile->mRecords.push_back(std::move(sequenceController));
    const std::vector<Render::Mat4> externalPose
        = Nif::collectBonePose(Nif::FileView(*kfFile), animatedBoneNames, 0.25f);
    if (externalPose.size() != 1)
        throw std::runtime_error("neutral KF pose sampler did not find the controller bone");
    expectNear(externalPose.front().data[12], 0.625f, "sampled external KF translation");
    const std::array<Nif::FileView, 2> layeredFiles{ Nif::FileView(*file), Nif::FileView(*kfFile) };
    const std::vector<Render::Mat4> layeredPose
        = Nif::collectBonePose(layeredFiles, animatedBoneNames, 0.25f);
    expectNear(layeredPose.front().data[12], 0.625f, "later layered KF source priority");
    const std::vector<Render::Mat4> groupedExternalPose
        = Nif::collectBonePose(Nif::FileView(*kfFile), animatedBoneNames, 0.25f, "idle");
    expectNear(groupedExternalPose.front().data[12], 2.f, "sampled grouped external KF translation");
    const std::vector<Render::Mat4> segmentedExternalPose
        = Nif::collectBonePose(Nif::FileView(*kfFile), animatedBoneNames, 0.25f, "idle", "mid", "stop");
    expectNear(segmentedExternalPose.front().data[12], 3.375f, "sampled segmented external KF translation");
    const std::vector<Render::Mat4> clampedExternalPose
        = Nif::collectBonePose(Nif::FileView(*kfFile), animatedBoneNames, 0.5f, "idle", "mid", "stop");
    expectNear(clampedExternalPose.front().data[12], 3.375f, "clamped segmented external KF translation");
    const std::vector<Render::AnimationTextKey> externalTextKeys
        = Nif::collectTextKeys(Nif::FileView(*kfFile), "idle");
    if (externalTextKeys.size() != 3 || externalTextKeys[0].event != "Idle: Start"
        || externalTextKeys[1].time != 0.5f || externalTextKeys[2].event != "Idle: Stop")
        throw std::runtime_error("neutral KF text-key events were not collected in time order");

    auto sequenceInterpolatorA = std::make_unique<Nif::NiTransformInterpolator>();
    sequenceInterpolatorA->mRecordType = Nif::RC_NiTransformInterpolator;
    sequenceInterpolatorA->mDefaultValue = Nif::NiQuatTransform::getIdentity();
    sequenceInterpolatorA->mDefaultValue.mTranslation.x() = 1.f;
    sequenceInterpolatorA->mData = nullptr;
    auto sequenceInterpolatorB = std::make_unique<Nif::NiTransformInterpolator>();
    sequenceInterpolatorB->mRecordType = Nif::RC_NiTransformInterpolator;
    sequenceInterpolatorB->mDefaultValue = Nif::NiQuatTransform::getIdentity();
    sequenceInterpolatorB->mDefaultValue.mTranslation.x() = 5.f;
    sequenceInterpolatorB->mData = nullptr;
    auto sequenceBlend = std::make_unique<Nif::NiBlendTransformInterpolator>();
    sequenceBlend->mRecordType = Nif::RC_NiBlendTransformInterpolator;
    sequenceBlend->mSingleInterpolator = nullptr;
    sequenceBlend->mItems.push_back({ sequenceInterpolatorA.get(), 0.25f, 0.25f, 0, 0.f });
    sequenceBlend->mItems.push_back({ sequenceInterpolatorB.get(), 0.75f, 0.75f, 0, 0.f });
    Nif::ControlledBlock sequenceBlock;
    sequenceBlock.mTargetName = "Root Bone";
    sequenceBlock.mInterpolator = nullptr;
    sequenceBlock.mController = nullptr;
    sequenceBlock.mBlendInterpolator = sequenceBlend.get();
    sequenceBlock.mBlendIndex = 0;
    sequenceBlock.mBlendIndexSet = true;
    auto controllerSequence = std::make_unique<Nif::NiControllerSequence>();
    controllerSequence->mRecordType = Nif::RC_NiControllerSequence;
    controllerSequence->mName = "idle";
    controllerSequence->mTextKeys = nullptr;
    controllerSequence->mControlledBlocks.push_back(sequenceBlock);
    auto lowerPriorityInterpolator = std::make_unique<Nif::NiTransformInterpolator>();
    lowerPriorityInterpolator->mRecordType = Nif::RC_NiTransformInterpolator;
    lowerPriorityInterpolator->mDefaultValue = Nif::NiQuatTransform::getIdentity();
    lowerPriorityInterpolator->mDefaultValue.mTranslation.x() = 9.f;
    lowerPriorityInterpolator->mData = nullptr;
    Nif::ControlledBlock lowerPriorityBlock = sequenceBlock;
    lowerPriorityBlock.mInterpolator = lowerPriorityInterpolator.get();
    lowerPriorityBlock.mBlendInterpolator = nullptr;
    lowerPriorityBlock.mPriority = 1;
    controllerSequence->mControlledBlocks.push_back(lowerPriorityBlock);
    sequenceBlock.mPriority = 3;
    controllerSequence->mControlledBlocks.front() = sequenceBlock;
    Nif::ControlledBlock excludedBlendBlock = sequenceBlock;
    excludedBlendBlock.mInterpolator = lowerPriorityInterpolator.get();
    excludedBlendBlock.mBlendInterpolator = nullptr;
    excludedBlendBlock.mBlendIndex = 2;
    excludedBlendBlock.mPriority = 255;
    controllerSequence->mControlledBlocks.push_back(excludedBlendBlock);
    controllerSequence->mWeight = 0.75f;
    auto secondSequenceInterpolator = std::make_unique<Nif::NiTransformInterpolator>();
    secondSequenceInterpolator->mRecordType = Nif::RC_NiTransformInterpolator;
    secondSequenceInterpolator->mDefaultValue = Nif::NiQuatTransform::getIdentity();
    secondSequenceInterpolator->mDefaultValue.mTranslation.x() = 8.f;
    secondSequenceInterpolator->mData = nullptr;
    Nif::ControlledBlock secondSequenceBlock;
    secondSequenceBlock.mTargetName = "Root Bone";
    secondSequenceBlock.mInterpolator = secondSequenceInterpolator.get();
    secondSequenceBlock.mController = nullptr;
    secondSequenceBlock.mBlendInterpolator = nullptr;
    secondSequenceBlock.mPriority = 3;
    auto secondControllerSequence = std::make_unique<Nif::NiControllerSequence>();
    secondControllerSequence->mRecordType = Nif::RC_NiControllerSequence;
    secondControllerSequence->mName = "idle";
    secondControllerSequence->mTextKeys = nullptr;
    secondControllerSequence->mWeight = 0.25f;
    secondControllerSequence->mControlledBlocks.push_back(secondSequenceBlock);
    auto controllerSequenceFile = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("synthetic-controller.kf"));
    controllerSequenceFile->mRoots.push_back(controllerSequence.get());
    controllerSequenceFile->mRoots.push_back(secondControllerSequence.get());
    controllerSequenceFile->mRecords.push_back(std::move(sequenceInterpolatorA));
    controllerSequenceFile->mRecords.push_back(std::move(sequenceInterpolatorB));
    controllerSequenceFile->mRecords.push_back(std::move(sequenceBlend));
    controllerSequenceFile->mRecords.push_back(std::move(lowerPriorityInterpolator));
    controllerSequenceFile->mRecords.push_back(std::move(secondSequenceInterpolator));
    controllerSequenceFile->mRecords.push_back(std::move(controllerSequence));
    controllerSequenceFile->mRecords.push_back(std::move(secondControllerSequence));
    const std::vector<Render::Mat4> controllerSequencePose
        = Nif::collectBonePose(Nif::FileView(*controllerSequenceFile), animatedBoneNames, 0.25f, "idle");
    if (controllerSequencePose.size() != 1)
        throw std::runtime_error("neutral controller sequence did not produce a bone pose");
    expectNear(controllerSequencePose.front().data[12], 5.f, "weighted controller sequence translation");
    const std::array<std::string, 1> lowerCaseBoneNames{ "root bone" };
    const std::vector<Render::Mat4> caseInsensitiveControllerPose
        = Nif::collectBonePose(Nif::FileView(*controllerSequenceFile), lowerCaseBoneNames, 0.25f, "idle");
    if (caseInsensitiveControllerPose.size() != 1)
        throw std::runtime_error("neutral controller sequence did not match bone names case-insensitively");
    expectNear(caseInsensitiveControllerPose.front().data[12], 5.f, "case-insensitive controller bone lookup");

    auto sequenceTimeInterpolator = std::make_unique<Nif::NiTransformInterpolator>();
    sequenceTimeInterpolator->mRecordType = Nif::RC_NiTransformInterpolator;
    sequenceTimeInterpolator->mDefaultValue = Nif::NiQuatTransform::getIdentity();
    sequenceTimeInterpolator->mData = animatedData.get();
    Nif::ControlledBlock sequenceTimeBlock;
    sequenceTimeBlock.mTargetName = "Root Bone";
    sequenceTimeBlock.mInterpolator = sequenceTimeInterpolator.get();
    sequenceTimeBlock.mController = nullptr;
    sequenceTimeBlock.mBlendInterpolator = nullptr;
    auto timedControllerSequence = std::make_unique<Nif::NiControllerSequence>();
    timedControllerSequence->mRecordType = Nif::RC_NiControllerSequence;
    timedControllerSequence->mName = "idle";
    timedControllerSequence->mTextKeys = nullptr;
    timedControllerSequence->mFrequency = 1.f;
    timedControllerSequence->mPhase = 0.f;
    timedControllerSequence->mStartTime = 0.f;
    timedControllerSequence->mStopTime = 1.f;
    timedControllerSequence->mExtrapolationMode = Nif::NiTimeController::ExtrapolationMode::Cycle;
    timedControllerSequence->mControlledBlocks.push_back(sequenceTimeBlock);
    auto* timedSequence = timedControllerSequence.get();
    auto timedControllerSequenceFile
        = std::make_shared<Nif::NIFFile>(VFS::Path::Normalized("synthetic-controller-time.kf"));
    timedControllerSequenceFile->mRoots.push_back(timedControllerSequence.get());
    timedControllerSequenceFile->mRecords.push_back(std::move(sequenceTimeInterpolator));
    timedControllerSequenceFile->mRecords.push_back(std::move(timedControllerSequence));
    const std::vector<Render::Mat4> cycledControllerSequencePose
        = Nif::collectBonePose(Nif::FileView(*timedControllerSequenceFile), animatedBoneNames, 1.25f, "idle");
    if (cycledControllerSequencePose.size() != 1)
        throw std::runtime_error("neutral controller sequence cycle did not produce a bone pose");
    expectNear(cycledControllerSequencePose.front().data[12], 0.625f,
        "cycled controller sequence time");
    timedSequence->mExtrapolationMode = Nif::NiTimeController::ExtrapolationMode::Reverse;
    const std::vector<Render::Mat4> reversedControllerSequencePose
        = Nif::collectBonePose(Nif::FileView(*timedControllerSequenceFile), animatedBoneNames, 1.25f, "idle");
    expectNear(reversedControllerSequencePose.front().data[12], 3.375f,
        "reversed controller sequence time");
    timedSequence->mExtrapolationMode = Nif::NiTimeController::ExtrapolationMode::Constant;
    timedSequence->mPlayBackwards = true;
    const std::vector<Render::Mat4> backwardsControllerSequencePose
        = Nif::collectBonePose(Nif::FileView(*timedControllerSequenceFile), animatedBoneNames, 0.25f, "idle");
    expectNear(backwardsControllerSequencePose.front().data[12], 3.375f,
        "backwards controller sequence time");
    timedSequence->mWeight = 0.f;
    if (!Nif::collectBonePose(Nif::FileView(*timedControllerSequenceFile), animatedBoneNames, 0.25f, "idle").empty())
        throw std::runtime_error("zero-weight controller sequence unexpectedly produced a pose");
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
        || !noLightingInstances.front().mesh.material.unlit
        || noLightingInstances.front().mesh.material.albedoWrapU
        || !noLightingInstances.front().mesh.material.albedoWrapV)
        throw std::runtime_error("NIF no-lighting shader material conversion lost neutral state");
    const Render::MeshBatch noLightingBatch = Render::batchMeshes(noLightingInstances);
    if (noLightingBatch.vertices.empty() || noLightingBatch.vertices.front().material[2] != 6.f)
        throw std::runtime_error("renderer-neutral no-lighting material lost its unlit mode");

    Resource::NifMeshManager meshManager(nullptr);
    const std::optional<float> externalDuration = meshManager.getAnimationDuration(kfFile);
    if (!externalDuration || *externalDuration != 1.f)
        throw std::runtime_error("neutral KF full duration was not resolved for effect playback");
    if (!meshManager.hasAnimationGroup(kfFile, "idle") || meshManager.hasAnimationGroup(kfFile, "missing"))
        throw std::runtime_error("neutral animation group presence was not resolved from text keys");
    if (meshManager.getAnimationDuration(kfFile, "missing", {}, {}))
        throw std::runtime_error("neutral animation duration accepted a missing group");
    const std::optional<float> groupedDuration = meshManager.getAnimationDuration(kfFile, "idle", "start", "stop");
    if (!groupedDuration || *groupedDuration != 0.5f)
        throw std::runtime_error("neutral KF text-key duration was not resolved");
    const std::vector<Render::AnimationTextKey> segmentedTextKeys
        = meshManager.getAnimationTextKeys(kfFile, "idle", "start", "stop");
    if (segmentedTextKeys.size() != 3 || segmentedTextKeys[0].time != 0.f || segmentedTextKeys[1].time != 0.25f
        || segmentedTextKeys[2].time != 0.5f)
        throw std::runtime_error("neutral KF text-key segment times were not rebased");
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
    expectNear(batch.vertices.front().material[1], 0.65f, "batched material specular strength");
    expectNear(batch.vertices.front().material[2], 3.f, "batched material shading mode");
    expectNear(batch.vertices.front().specular[0], 0.2f, "batched material specular red");
    expectNear(batch.vertices.front().specular[2], 0.8f, "batched material specular blue");
    expectNear(batch.vertices.front().material[3], 0.6f, "batched material emission");
    expectNear(batch.vertices.front().emissive[0], 0.2f, "batched emissive red");
    expectNear(batch.vertices.front().emissive[2], 0.6f, "batched emissive blue");
    expectNear(batch.draws[1].transform.data[12], 12.0f, "batched mesh translation");

    Render::WorldObject object;
    object.id = 1;
    object.model = "synthetic.nif";
    object.visible = true;
    object.dynamic = false;
    object.transform.position.x = 5.0f;
    const Render::MeshInstance transformed = Render::transformMeshInstance(object, instances.front());
    expectNear(transformed.transform.data[12], 17.0f, "cell object translation");

    Render::CellScene scene;
    Render::WorldObject hidden;
    hidden.id = 2;
    hidden.model = "hidden.nif";
    hidden.visible = false;
    scene.objects.push_back(std::move(hidden));
    Render::WorldObject visible;
    visible.id = 3;
    visible.model = "synthetic.nif";
    scene.objects.push_back(std::move(visible));
    Render::WorldObject animated;
    animated.id = 4;
    animated.model = "animated.nif";
    animated.dynamic = true;
    scene.objects.push_back(std::move(animated));
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
