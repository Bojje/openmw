#include <cmath>
#include <stdexcept>

#include <components/render/meshconversion.hpp>
#include <components/render/animationmask.hpp>
#include <components/render/stats.hpp>

int main()
{
    Render::BasicFrameStats frameStats;
    if (frameStats.collectStats("engine"))
        throw std::runtime_error("basic Vulkan frame stats must default to disabled collection");
    frameStats.setAttribute(1, "neutral-test", 1.0);
    frameStats.setCollectStats(true);
    if (!frameStats.collectStats("engine"))
        throw std::runtime_error("basic Vulkan frame stats did not enable collection");

    if (Render::classifyAnimationBone("Bip01 Spine1") != Render::AnimationBoneGroup::Torso
        || Render::classifyAnimationBone("BIP01 L Hand") != Render::AnimationBoneGroup::LeftArm
        || Render::classifyAnimationBone("weapon bone") != Render::AnimationBoneGroup::RightArm
        || !Render::animationBoneInMask("Shield Bone", Render::AnimationMask_LeftArm)
        || Render::animationBoneInMask("Bip01 L Hand", Render::AnimationMask_RightArm))
        throw std::runtime_error("renderer-neutral animation bone masks are inconsistent");

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
