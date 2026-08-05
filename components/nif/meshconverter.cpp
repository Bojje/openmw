#include "meshconverter.hpp"

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
            }

            return result;
        }

        void appendIndex(Render::MeshData& mesh, uint32_t index)
        {
            if (index >= mesh.vertices.size())
                throw std::runtime_error("NIF triangle index is outside the vertex data");
            mesh.indices.push_back(index);
        }

        void setShaderTexture(Render::MeshMaterial& material, const BSShaderTextureSetPtr& textureSet)
        {
            if (!textureSet.empty() && !textureSet->mTextures.empty())
                material.albedoTexture = VFS::Path::toNormalized(textureSet->mTextures.front()).value();
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
                            result.albedoTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
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
                    setShaderTexture(result, lighting->mTextureSet);
                    result.diffuse.w = lighting->mAlpha;
                    result.emissive = { lighting->mEmissive.x() * lighting->mEmissiveMult,
                        lighting->mEmissive.y() * lighting->mEmissiveMult,
                        lighting->mEmissive.z() * lighting->mEmissiveMult, 1.f };
                    result.glossiness = lighting->mGlossiness;
                }
                else if (const auto* ppLighting = dynamic_cast<const BSShaderPPLightingProperty*>(shader))
                {
                    setShaderTexture(result, ppLighting->mTextureSet);
                    result.emissive = { ppLighting->mEmissiveColor.x(), ppLighting->mEmissiveColor.y(),
                        ppLighting->mEmissiveColor.z(), ppLighting->mEmissiveColor.w() };
                }
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
            std::vector<Render::MeshInstance>& meshes)
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
                        meshes.push_back({ std::move(mesh), transform });
                    }
                    else if (const auto* stripsData = dynamic_cast<const NiTriStripsData*>(&geometry->mData.get()))
                    {
                        Render::MeshData mesh = convertMesh(*stripsData);
                        mesh.material = convertMaterial(*geometry);
                        meshes.push_back({ std::move(mesh), transform });
                    }
                }
            }

            if (const auto* node = dynamic_cast<const NiNode*>(&object))
            {
                for (const auto& child : node->mChildren)
                {
                    if (!child.empty())
                        collectMeshInstances(*child.getPtr(), transform, meshes);
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
                collectMeshInstances(*root, identity, meshes);
        }
        return meshes;
    }
}
