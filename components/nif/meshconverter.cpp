#include "meshconverter.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "data.hpp"
#include "node.hpp"
#include "property.hpp"
#include "texture.hpp"
#include <components/render/math.hpp>
#include <components/vfs/pathutil.hpp>

namespace Nif
{
    namespace
    {
        Render::Mat4 toRenderMatrix(const NiTransform& transform);

        Render::MeshData convertVertices(const NiGeometryData& source)
        {
            Render::MeshData result;
            result.vertices.resize(source.mVertices.size());

            const bool hasNormals = source.mNormals.size() == source.mVertices.size();
            const bool hasColors = source.mColors.size() == source.mVertices.size();
            const bool hasTexcoords = !source.mUVList.empty()
                && source.mUVList.front().size() == source.mVertices.size();

            for (std::size_t i = 0; i < source.mVertices.size(); ++i)
            {
                const auto& position = source.mVertices[i];
                auto& vertex = result.vertices[i];
                vertex.position[0] = position.x();
                vertex.position[1] = position.y();
                vertex.position[2] = position.z();

                if (hasNormals)
                {
                    const auto& normal = source.mNormals[i];
                    vertex.normal[0] = normal.x();
                    vertex.normal[1] = normal.y();
                    vertex.normal[2] = normal.z();
                }
                else
                {
                    vertex.normal[0] = 0.0f;
                    vertex.normal[1] = 0.0f;
                    vertex.normal[2] = 1.0f;
                }

                if (hasTexcoords)
                {
                    const auto& texcoord = source.mUVList.front()[i];
                    vertex.texcoord[0] = texcoord.x();
                    vertex.texcoord[1] = texcoord.y();
                }
                else
                {
                    vertex.texcoord[0] = 0.0f;
                    vertex.texcoord[1] = 0.0f;
                }
                vertex.blendTexcoord[0] = vertex.texcoord[0];
                vertex.blendTexcoord[1] = vertex.texcoord[1];

                if (hasColors)
                {
                    const auto& color = source.mColors[i];
                    vertex.color[0] = color.x();
                    vertex.color[1] = color.y();
                    vertex.color[2] = color.z();
                    vertex.color[3] = color.w();
                }
                else
                {
                    vertex.color[0] = 1.0f;
                    vertex.color[1] = 1.0f;
                    vertex.color[2] = 1.0f;
                    vertex.color[3] = 1.0f;
                }

                vertex.material[0] = 1.0f;
                vertex.material[1] = 0.0f;
                vertex.material[2] = 1.0f;
                vertex.material[3] = 0.0f;
                vertex.tangent[0] = 1.0f;
                vertex.tangent[1] = 0.0f;
                vertex.tangent[2] = 0.0f;
                vertex.tangent[3] = 1.0f;
            }

            return result;
        }

        void appendIndex(Render::MeshData& mesh, uint32_t index)
        {
            if (index >= mesh.vertices.size())
                throw std::runtime_error("NIF triangle index is outside the vertex data");
            mesh.indices.push_back(index);
        }

        void computeTangents(Render::MeshData& mesh)
        {
            std::vector<std::array<float, 3>> tangents(mesh.vertices.size());
            std::vector<std::array<float, 3>> bitangents(mesh.vertices.size());

            const auto add = [](std::array<float, 3>& target, const std::array<float, 3>& value) {
                for (std::size_t axis = 0; axis < 3; ++axis)
                    target[axis] += value[axis];
            };
            const auto edge = [&mesh](uint32_t index, uint32_t origin) {
                return std::array<float, 3>{ mesh.vertices[index].position[0] - mesh.vertices[origin].position[0],
                    mesh.vertices[index].position[1] - mesh.vertices[origin].position[1],
                    mesh.vertices[index].position[2] - mesh.vertices[origin].position[2] };
            };

            for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
            {
                const uint32_t i0 = mesh.indices[triangle + 0];
                const uint32_t i1 = mesh.indices[triangle + 1];
                const uint32_t i2 = mesh.indices[triangle + 2];
                const float du1 = mesh.vertices[i1].texcoord[0] - mesh.vertices[i0].texcoord[0];
                const float dv1 = mesh.vertices[i1].texcoord[1] - mesh.vertices[i0].texcoord[1];
                const float du2 = mesh.vertices[i2].texcoord[0] - mesh.vertices[i0].texcoord[0];
                const float dv2 = mesh.vertices[i2].texcoord[1] - mesh.vertices[i0].texcoord[1];
                const float determinant = du1 * dv2 - du2 * dv1;
                if (std::abs(determinant) < 1e-6f)
                    continue;

                const std::array<float, 3> edge1 = edge(i1, i0);
                const std::array<float, 3> edge2 = edge(i2, i0);
                const float inverseDeterminant = 1.f / determinant;
                const std::array<float, 3> tangent = {
                    (edge1[0] * dv2 - edge2[0] * dv1) * inverseDeterminant,
                    (edge1[1] * dv2 - edge2[1] * dv1) * inverseDeterminant,
                    (edge1[2] * dv2 - edge2[2] * dv1) * inverseDeterminant,
                };
                const std::array<float, 3> bitangent = {
                    (edge2[0] * du1 - edge1[0] * du2) * inverseDeterminant,
                    (edge2[1] * du1 - edge1[1] * du2) * inverseDeterminant,
                    (edge2[2] * du1 - edge1[2] * du2) * inverseDeterminant,
                };
                add(tangents[i0], tangent);
                add(tangents[i1], tangent);
                add(tangents[i2], tangent);
                add(bitangents[i0], bitangent);
                add(bitangents[i1], bitangent);
                add(bitangents[i2], bitangent);
            }

            for (std::size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex)
            {
                Render::MeshVertex& vertex = mesh.vertices[vertexIndex];
                const std::array<float, 3> normal = { vertex.normal[0], vertex.normal[1], vertex.normal[2] };
                std::array<float, 3> tangent = tangents[vertexIndex];
                const float normalLength = std::sqrt(
                    normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
                const std::array<float, 3> unitNormal = normalLength > 1e-6f
                    ? std::array<float, 3>{ normal[0] / normalLength, normal[1] / normalLength,
                          normal[2] / normalLength }
                    : std::array<float, 3>{ 0.f, 0.f, 1.f };
                const float projection = tangent[0] * unitNormal[0] + tangent[1] * unitNormal[1]
                    + tangent[2] * unitNormal[2];
                for (std::size_t axis = 0; axis < 3; ++axis)
                    tangent[axis] -= unitNormal[axis] * projection;

                float tangentLength = std::sqrt(
                    tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
                if (tangentLength <= 1e-6f)
                {
                    const std::array<float, 3> axis = std::abs(unitNormal[2]) < 0.9f
                        ? std::array<float, 3>{ 0.f, 0.f, 1.f }
                        : std::array<float, 3>{ 0.f, 1.f, 0.f };
                    tangent = { axis[1] * unitNormal[2] - axis[2] * unitNormal[1],
                        axis[2] * unitNormal[0] - axis[0] * unitNormal[2],
                        axis[0] * unitNormal[1] - axis[1] * unitNormal[0] };
                    tangentLength = std::sqrt(
                        tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
                }
                for (float& component : tangent)
                    component /= tangentLength;

                const std::array<float, 3> bitangent = {
                    unitNormal[1] * tangent[2] - unitNormal[2] * tangent[1],
                    unitNormal[2] * tangent[0] - unitNormal[0] * tangent[2],
                    unitNormal[0] * tangent[1] - unitNormal[1] * tangent[0],
                };
                const float handedness = bitangent[0] * bitangents[vertexIndex][0]
                        + bitangent[1] * bitangents[vertexIndex][1]
                        + bitangent[2] * bitangents[vertexIndex][2]
                    < 0.f
                    ? -1.f
                    : 1.f;
                vertex.tangent[0] = tangent[0];
                vertex.tangent[1] = tangent[1];
                vertex.tangent[2] = tangent[2];
                vertex.tangent[3] = handedness;
            }
        }

        void setShaderTexture(Render::MeshMaterial& material, const BSShaderTextureSetPtr& textureSet,
            bool wrapU, bool wrapV)
        {
            if (textureSet.empty() || textureSet->mTextures.empty())
                return;

            material.albedoTexture = VFS::Path::toNormalized(textureSet->mTextures.front()).value();
            material.albedoWrapU = wrapU;
            material.albedoWrapV = wrapV;
            if (textureSet->mTextures.size() > 1 && !textureSet->mTextures[1].empty())
            {
                material.normalTexture = VFS::Path::toNormalized(textureSet->mTextures[1]).value();
                material.normalMap = !material.normalTexture.empty();
                material.normalWrapU = wrapU;
                material.normalWrapV = wrapV;
            }
            if (textureSet->mTextures.size() > 2 && !textureSet->mTextures[2].empty())
            {
                material.emissiveTexture = VFS::Path::toNormalized(textureSet->mTextures[2]).value();
                material.emissiveWrapU = wrapU;
                material.emissiveWrapV = wrapV;
            }
        }

        Render::MeshMaterial convertMaterial(const NiGeometry& geometry)
        {
            Render::MeshMaterial result;

            for (const auto& property : geometry.mProperties)
            {
                if (property.empty())
                    continue;

                if (const auto* texturing = dynamic_cast<const NiTexturingProperty*>(property.getPtr()))
                {
                    if (texturing->mTextures.size() > NiTexturingProperty::BaseTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::BaseTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.albedoTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.albedoWrapU = texture.wrapS();
                            result.albedoWrapV = texture.wrapT();
                        }
                    }
                    if (texturing->mTextures.size() > NiTexturingProperty::BumpTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::BumpTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.normalTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.normalWrapU = texture.wrapS();
                            result.normalWrapV = texture.wrapT();
                        }
                    }
                    if (texturing->mTextures.size() > NiTexturingProperty::GlowTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::GlowTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.emissiveTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.emissiveWrapU = texture.wrapS();
                            result.emissiveWrapV = texture.wrapT();
                        }
                    }
                }
                else if (const auto* material = dynamic_cast<const NiMaterialProperty*>(property.getPtr()))
                {
                    result.diffuse = { material->mDiffuse.x(), material->mDiffuse.y(), material->mDiffuse.z(),
                        material->mAlpha };
                    result.emissive = { material->mEmissive.x() * material->mEmissiveMult,
                        material->mEmissive.y() * material->mEmissiveMult,
                        material->mEmissive.z() * material->mEmissiveMult, 1.f };
                    result.glossiness = material->mGlossiness;
                }
            }

            if (!geometry.mAlphaProperty.empty())
            {
                const NiAlphaProperty& alpha = *geometry.mAlphaProperty.getPtr();
                result.alphaBlend = alpha.useAlphaBlending();
                result.alphaTest = alpha.useAlphaTesting();
                result.alphaTestThreshold = alpha.mThreshold;
            }

            if (!geometry.mShaderProperty.empty())
            {
                const BSShaderProperty* shader = geometry.mShaderProperty.getPtr();
                if (const auto* lighting = dynamic_cast<const BSLightingShaderProperty*>(shader))
                {
                    setShaderTexture(result, lighting->mTextureSet, lighting->wrapS(), lighting->wrapT());
                    result.doubleSided = lighting->doubleSided();
                    result.diffuse.w = lighting->mAlpha;
                    result.emissive = { lighting->mEmissive.x() * lighting->mEmissiveMult,
                        lighting->mEmissive.y() * lighting->mEmissiveMult,
                        lighting->mEmissive.z() * lighting->mEmissiveMult, 1.f };
                    result.glossiness = lighting->mGlossiness;
                }
                else if (const auto* ppLighting = dynamic_cast<const BSShaderPPLightingProperty*>(shader))
                {
                    setShaderTexture(result, ppLighting->mTextureSet, ppLighting->wrapS(), ppLighting->wrapT());
                    result.emissive = { ppLighting->mEmissiveColor.x(), ppLighting->mEmissiveColor.y(),
                        ppLighting->mEmissiveColor.z(), ppLighting->mEmissiveColor.w() };
                }
                else if (const auto* noLighting = dynamic_cast<const BSShaderNoLightingProperty*>(shader))
                {
                    if (!noLighting->mFilename.empty())
                    {
                        result.albedoTexture = VFS::Path::toNormalized(noLighting->mFilename).value();
                        result.albedoWrapU = noLighting->wrapS();
                        result.albedoWrapV = noLighting->wrapT();
                    }
                }
                else if (const auto* effect = dynamic_cast<const BSEffectShaderProperty*>(shader))
                {
                    if (!effect->mSourceTexture.empty())
                    {
                        result.albedoTexture = VFS::Path::toNormalized(effect->mSourceTexture).value();
                        result.albedoWrapU = effect->wrapS();
                        result.albedoWrapV = effect->wrapT();
                    }
                    if (!effect->mNormalTexture.empty())
                    {
                        result.normalTexture = VFS::Path::toNormalized(effect->mNormalTexture).value();
                        result.normalWrapU = effect->wrapS();
                        result.normalWrapV = effect->wrapT();
                    }
                    result.diffuse = { effect->mBaseColor.x() * effect->mBaseColorScale,
                        effect->mBaseColor.y() * effect->mBaseColorScale,
                        effect->mBaseColor.z() * effect->mBaseColorScale, effect->mBaseColor.w() };
                    result.emissive = { effect->mEmittanceColor.x(), effect->mEmittanceColor.y(),
                        effect->mEmittanceColor.z(), 1.f };
                    result.doubleSided = effect->doubleSided();
                    result.alphaBlend = result.diffuse.w < 1.f || effect->softEffect() || effect->refraction();
                }
            }

            result.normalMap = !result.normalTexture.empty();

            return result;
        }

        std::shared_ptr<const Render::SkinningData> convertSkinning(const NiGeometry& geometry,
            std::size_t vertexCount)
        {
            if (geometry.mSkin.empty() || geometry.mSkin->mData.empty())
                return {};

            const NiSkinData& source = *geometry.mSkin->mData.getPtr();
            if (source.mBones.empty() || geometry.mSkin->mBones.size() != source.mBones.size())
                return {};

            auto result = std::make_shared<Render::SkinningData>();
            result->vertices.resize(vertexCount);
            result->inverseBindMatrices.reserve(source.mBones.size());
            for (const NiSkinData::BoneInfo& bone : source.mBones)
                result->inverseBindMatrices.push_back(toRenderMatrix(bone.mTransform));

            for (std::size_t boneIndex = 0; boneIndex < source.mBones.size(); ++boneIndex)
            {
                for (const auto [vertexIndex, weight] : source.mBones[boneIndex].mWeights)
                {
                    if (vertexIndex >= result->vertices.size() || !std::isfinite(weight) || weight <= 0.f)
                        continue;

                    Render::SkinVertex& vertex = result->vertices[vertexIndex];
                    std::size_t slot = 0;
                    for (std::size_t influence = 1; influence < vertex.weights.size(); ++influence)
                    {
                        if (vertex.weights[influence] < vertex.weights[slot])
                            slot = influence;
                    }
                    if (weight > vertex.weights[slot])
                    {
                        vertex.boneIndices[slot] = static_cast<std::uint16_t>(boneIndex);
                        vertex.weights[slot] = weight;
                    }
                }
            }

            for (Render::SkinVertex& vertex : result->vertices)
            {
                float total = 0.f;
                for (const float weight : vertex.weights)
                    total += weight;
                if (total <= 0.f)
                    return {};
                for (float& weight : vertex.weights)
                    weight /= total;
            }
            return result;
        }
    }

    Render::MeshData convertMesh(const NiTriShapeData& source)
    {
        if (source.mTriangles.size() % 3 != 0)
            throw std::runtime_error("NIF triangle index data is not a multiple of three");

        Render::MeshData result = convertVertices(source);
        result.indices.reserve(source.mTriangles.size());
        for (unsigned short index : source.mTriangles)
            appendIndex(result, index);
        computeTangents(result);
        return result;
    }

    Render::MeshData convertMesh(const NiTriStripsData& source)
    {
        Render::MeshData result = convertVertices(source);
        for (const std::vector<unsigned short>& strip : source.mStrips)
        {
            if (strip.size() < 3)
                continue;

            result.indices.reserve(result.indices.size() + (strip.size() - 2) * 3);
            for (std::size_t i = 2; i < strip.size(); ++i)
            {
                const unsigned short a = strip[i - 2];
                const unsigned short b = strip[i - 1];
                const unsigned short c = strip[i];
                if (a == b || b == c || a == c)
                    continue;
                if (i % 2 == 0)
                {
                    appendIndex(result, a);
                    appendIndex(result, b);
                    appendIndex(result, c);
                }
                else
                {
                    appendIndex(result, a);
                    appendIndex(result, c);
                    appendIndex(result, b);
                }
            }
        }

        computeTangents(result);
        return result;
    }

    namespace
    {
        Render::Mat4 toRenderMatrix(const NiTransform& transform)
        {
            Render::Mat4 result = {};
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                    result.data[col * 4 + row] = transform.mRotation.mValues[row][col] * transform.mScale;
            }
            result.data[12] = transform.mTranslation.x();
            result.data[13] = transform.mTranslation.y();
            result.data[14] = transform.mTranslation.z();
            result.data[15] = 1.0f;
            return result;
        }

        void collectMeshInstances(const NiAVObject& object, const Render::Mat4& parentTransform,
            std::vector<Render::MeshInstance>& meshes, bool allowSkinning)
        {
            const Render::Mat4 transform = Render::multiply(parentTransform, toRenderMatrix(object.mTransform));
            if (const auto* geometry = dynamic_cast<const NiGeometry*>(&object))
            {
                if (!geometry->mData.empty())
                {
                    if (const auto* shapeData = dynamic_cast<const NiTriShapeData*>(&geometry->mData.get()))
                    {
                        Render::MeshData mesh = convertMesh(*shapeData);
                        mesh.material = convertMaterial(*geometry);
                        if (allowSkinning)
                            mesh.skinning = convertSkinning(*geometry, mesh.vertices.size());
                        meshes.push_back({ std::move(mesh), transform });
                    }
                    else if (const auto* stripsData = dynamic_cast<const NiTriStripsData*>(&geometry->mData.get()))
                    {
                        Render::MeshData mesh = convertMesh(*stripsData);
                        mesh.material = convertMaterial(*geometry);
                        if (allowSkinning)
                            mesh.skinning = convertSkinning(*geometry, mesh.vertices.size());
                        meshes.push_back({ std::move(mesh), transform });
                    }
                }
            }

            if (const auto* node = dynamic_cast<const NiNode*>(&object))
            {
                for (const auto& child : node->mChildren)
                {
                    if (!child.empty())
                        collectMeshInstances(*child.getPtr(), transform, meshes, allowSkinning);
                }
            }
        }
    }

    std::vector<Render::MeshInstance> collectMeshInstances(FileView file)
    {
        Render::Mat4 identity = {};
        identity.data[0] = 1.0f;
        identity.data[5] = 1.0f;
        identity.data[10] = 1.0f;
        identity.data[15] = 1.0f;

        std::vector<Render::MeshInstance> meshes;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
        {
            if (const auto* root = dynamic_cast<const NiAVObject*>(file.getRoot(i)))
                collectMeshInstances(*root, identity, meshes, file.getUseSkinning());
        }
        return meshes;
    }
}
