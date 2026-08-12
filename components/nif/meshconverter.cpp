#include "meshconverter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>

#include "data.hpp"
#include "node.hpp"
#include "property.hpp"
#include "texture.hpp"
#include <components/render/meshconversion.hpp>
#include <components/render/math.hpp>
#include <components/vfs/pathutil.hpp>

namespace Nif
{
    namespace
    {
        Render::Mat4 toRenderMatrix(const NiTransform& transform);

        std::vector<Render::MeshVertexSource> convertVertices(const NiGeometryData& source)
        {
            std::vector<Render::MeshVertexSource> result(source.mVertices.size());

            const bool hasNormals = source.mNormals.size() == source.mVertices.size();
            const bool hasColors = source.mColors.size() == source.mVertices.size();
            const bool hasTexcoords = !source.mUVList.empty()
                && source.mUVList.front().size() == source.mVertices.size();

            for (std::size_t i = 0; i < source.mVertices.size(); ++i)
            {
                const auto& position = source.mVertices[i];
                auto& vertex = result[i];
                vertex.position[0] = position.x();
                vertex.position[1] = position.y();
                vertex.position[2] = position.z();

                if (hasNormals)
                {
                    const auto& normal = source.mNormals[i];
                    vertex.normal[0] = normal.x();
                    vertex.normal[1] = normal.y();
                    vertex.normal[2] = normal.z();
                    vertex.hasNormal = true;
                }

                if (hasTexcoords)
                {
                    const auto& texcoord = source.mUVList.front()[i];
                    vertex.texcoord[0] = texcoord.x();
                    vertex.texcoord[1] = texcoord.y();
                    vertex.hasTexcoord = true;
                }

                if (hasColors)
                {
                    const auto& color = source.mColors[i];
                    vertex.color[0] = color.x();
                    vertex.color[1] = color.y();
                    vertex.color[2] = color.z();
                    vertex.color[3] = color.w();
                    vertex.hasColor = true;
                }
            }

            return result;
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
            const bool hasBoneNames = std::all_of(geometry.mSkin->mBones.begin(), geometry.mSkin->mBones.end(),
                [](const auto& bone) { return !bone.empty() && !bone->mName.empty(); });
            if (hasBoneNames)
                result->boneNames.reserve(geometry.mSkin->mBones.size());
            result->inverseBindMatrices.reserve(source.mBones.size());
            for (std::size_t boneIndex = 0; boneIndex < source.mBones.size(); ++boneIndex)
            {
                const auto& bone = source.mBones[boneIndex];
                if (hasBoneNames)
                    result->boneNames.push_back(geometry.mSkin->mBones[boneIndex]->mName);
                result->inverseBindMatrices.push_back(toRenderMatrix(bone.mTransform));
            }

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

        const std::vector<Render::MeshVertexSource> vertices = convertVertices(source);
        Render::MeshData result = Render::makeMeshData(vertices);
        result.indices.reserve(source.mTriangles.size());
        for (unsigned short index : source.mTriangles)
            Render::appendMeshIndex(result, index);
        Render::computeMeshTangents(result);
        return result;
    }

    Render::MeshData convertMesh(const NiTriStripsData& source)
    {
        const std::vector<Render::MeshVertexSource> vertices = convertVertices(source);
        Render::MeshData result = Render::makeMeshData(vertices);
        for (const std::vector<unsigned short>& strip : source.mStrips)
            Render::appendTriangleStripIndices(result, strip);

        Render::computeMeshTangents(result);
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
