#include "meshconverter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <components/debug/debuglog.hpp>
#include <components/nif/data.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/vk/vkdevice.hpp>
#include <components/vk/vkgeometry.hpp>
#include <components/vk/vkmath.hpp>

namespace
{

    // Convert NiTransform (3x3 rotation + vec3 translation + float scale) to column-major 4x4
    void nifTransformToMat4(const Nif::NiTransform& t, float out[16])
    {
        // Column-major layout: out[col*4 + row]
        // Upper-left 3x3 = rotation * scale
        // Matrix3::mValues[i][j] is row i, column j of the rotation matrix
        for (int col = 0; col < 3; ++col)
        {
            for (int row = 0; row < 3; ++row)
                out[col * 4 + row] = t.mRotation.mValues[row][col] * t.mScale;
        }

        // Fourth column = translation
        out[12] = t.mTranslation.x();
        out[13] = t.mTranslation.y();
        out[14] = t.mTranslation.z();

        // Bottom row for columns 0-2
        out[3] = 0.f;
        out[7] = 0.f;
        out[11] = 0.f;

        // Bottom-right
        out[15] = 1.f;
    }

    // Convert triangle strips to a triangle list.
    // A strip of N vertices produces N-2 triangles. For triangle i:
    //   even: (v[i], v[i+1], v[i+2])
    //   odd:  (v[i+1], v[i], v[i+2])
    // Degenerate triangles (any two indices equal) are skipped.
    std::vector<uint32_t> convertStripsToTriangles(const std::vector<std::vector<unsigned short>>& strips)
    {
        std::vector<uint32_t> indices;
        for (const auto& strip : strips)
        {
            if (strip.size() < 3)
                continue;

            for (size_t i = 0; i + 2 < strip.size(); ++i)
            {
                uint32_t a, b, c;
                if (i % 2 == 0)
                {
                    a = strip[i];
                    b = strip[i + 1];
                    c = strip[i + 2];
                }
                else
                {
                    a = strip[i + 1];
                    b = strip[i];
                    c = strip[i + 2];
                }

                // Skip degenerate triangles
                if (a == b || b == c || a == c)
                    continue;

                indices.push_back(a);
                indices.push_back(b);
                indices.push_back(c);
            }
        }
        return indices;
    }

    // Base (diffuse) texture filename from a shape's NiTexturingProperty, or empty if it has none.
    // Only the base stage is read; dark/detail/glow/bump stages are not supported by the Vulkan
    // G-buffer yet. Mirrors the stage selection in nifosg::Loader::handleTextureProperty.
    std::string findBaseTexture(const Nif::NiAVObject* node)
    {
        for (const auto& propertyPtr : node->mProperties)
        {
            if (propertyPtr.empty())
                continue;

            const auto* texturing = dynamic_cast<const Nif::NiTexturingProperty*>(propertyPtr.getPtr());
            if (texturing == nullptr)
                continue;

            if (Nif::NiTexturingProperty::BaseTexture >= texturing->mTextures.size())
                continue;

            const Nif::NiTexturingProperty::Texture& base
                = texturing->mTextures[Nif::NiTexturingProperty::BaseTexture];
            if (!base.mEnabled || base.mSourceTexture.empty())
                continue;

            return base.mSourceTexture.getPtr()->mFile;
        }

        return {};
    }

    // Whether this shape's silhouette lives in its texture's alpha channel rather than in its
    // triangles, which decides whether the ray tracer may treat it as solid.
    //
    // Properties are inherited: nifosg's collectDrawableProperties walks the parent chain before the
    // shape's own properties, so a NiAlphaProperty on an ancestor NiNode still governs the drawable
    // below it. findBaseTexture above does not do this, which is a pre-existing limitation.
    //
    // Any NiAlphaProperty counts, whether it enables testing (bit 9) or blending (bit 0) -- a blended
    // shape is no more solid than a tested one. The threshold and comparison mode are ignored on
    // purpose; only the always-passes case would change the answer and it is not worth trusting the
    // file for. Erring towards true is nearly free: a false positive only costs traversal time,
    // whereas a false negative puts a solid rectangular shadow under a leaf billboard.
    //
    // Known gap: Morrowind ships some cut-out foliage with no NiAlphaProperty at all, relying on the
    // texture's alpha alone. The signal that would catch it is the pixel format -- most diffuse maps
    // are BC1_RGB and sample a == 1 -- but the texture is not resolved until after conversion has
    // already built the BLAS, so it cannot reach here today.
    bool findAlphaTested(const Nif::NiAVObject* node)
    {
        for (const auto& propertyPtr : node->mProperties)
        {
            if (propertyPtr.empty())
                continue;

            if (dynamic_cast<const Nif::NiAlphaProperty*>(propertyPtr.getPtr()) != nullptr)
                return true;
        }

        for (const Nif::NiNode* parent : node->mParents)
        {
            if (parent != nullptr && findAlphaTested(parent))
                return true;
        }

        return false;
    }

    // Nearest property of the given type affecting this shape, or nullptr if there is none.
    //
    // Properties are inherited and the deepest one wins: nifosg's collectDrawableProperties pushes the
    // parent chain's properties before the shape's own, and applyDrawableProperties then overwrites its
    // working state as it walks that list, so the last -- deepest -- property of a given type is the one
    // that survives. Searching the shape's own properties before recursing into mParents picks the same
    // winner without having to build the list first. Same parent walk as findAlphaTested above.
    template <class T>
    const T* findProperty(const Nif::NiAVObject* node)
    {
        for (const auto& propertyPtr : node->mProperties)
        {
            if (propertyPtr.empty())
                continue;

            const auto* property = dynamic_cast<const T*>(propertyPtr.getPtr());
            if (property != nullptr)
                return property;
        }

        for (const Nif::NiNode* parent : node->mParents)
        {
            if (parent == nullptr)
                continue;

            const T* property = findProperty<T>(parent);
            if (property != nullptr)
                return property;
        }

        return nullptr;
    }

    // Rec. 709 relative luminance. Same weights the engine already uses to collapse a colour to one
    // number, see files/shaders/compatibility/luminance/luminance.frag and the pR/pG/pB constants in
    // apps/openmw/mwrender/renderingmanager.cpp, so the Vulkan path does not invent a second convention.
    float luminance(const osg::Vec3f& colour)
    {
        return 0.2126f * colour.x() + 0.7152f * colour.y() + 0.0722f * colour.z();
    }

}

namespace NifVk
{

    MeshConverter::MeshConverter(Vk::Device& device, Vk::CommandPool& commandPool)
        : mDevice(device)
        , mCommandPool(commandPool)
    {
    }

    std::vector<VulkanMesh> MeshConverter::convert(const Nif::FileView& nif)
    {
        std::vector<VulkanMesh> meshes;

        mNifVersion = nif.getVersion();

        float identity[16];
        Vk::identityMat4(identity);

        for (size_t i = 0; i < nif.numRoots(); ++i)
        {
            const Nif::Record* root = nif.getRoot(i);
            if (root == nullptr)
                continue;

            const auto* avObject = dynamic_cast<const Nif::NiAVObject*>(root);
            if (avObject != nullptr)
                processNode(avObject, identity, meshes);
        }

        return meshes;
    }

    void MeshConverter::processNode(
        const Nif::NiAVObject* node, const float parentTransform[16], std::vector<VulkanMesh>& output)
    {
        if (node == nullptr || node->isHidden())
            return;

        // Compute world transform = parent * node's local transform
        float localMat[16];
        nifTransformToMat4(node->mTransform, localMat);

        float worldTransform[16];
        Vk::multiplyMat4(parentTransform, localMat, worldTransform);

        // Check if this is geometry (NiTriShape or NiTriStrips)
        if (node->mRecordType == Nif::RC_NiTriShape || node->mRecordType == Nif::RC_NiTriStrips
            || node->mRecordType == Nif::RC_BSSegmentedTriShape || node->mRecordType == Nif::RC_BSLODTriShape)
        {
            const auto* geom = static_cast<const Nif::NiGeometry*>(node);
            if (!geom->mSkin.empty())
            {
                Log(Debug::Warning) << "Vulkan: skinned mesh not yet supported, converting in bind pose";
            }
            if (!geom->mData.empty())
            {
                VulkanMesh mesh = processGeometry(geom, worldTransform);
                if (mesh.indexCount > 0)
                    output.push_back(std::move(mesh));
            }
            return;
        }

        // Check for switch node — only convert the active child
        const auto* switchNode = dynamic_cast<const Nif::NiSwitchNode*>(node);
        if (switchNode != nullptr)
        {
            uint32_t activeIndex = switchNode->mInitialIndex;
            if (activeIndex < switchNode->mChildren.size() && !switchNode->mChildren[activeIndex].empty())
                processNode(switchNode->mChildren[activeIndex].getPtr(), worldTransform, output);
            return;
        }

        // Check for LOD node — only convert the highest detail level
        const auto* lodNode = dynamic_cast<const Nif::NiLODNode*>(node);
        if (lodNode != nullptr)
        {
            if (!lodNode->mChildren.empty() && !lodNode->mChildren[0].empty())
                processNode(lodNode->mChildren[0].getPtr(), worldTransform, output);
            return;
        }

        // Check if this is a group node with children
        const auto* niNode = dynamic_cast<const Nif::NiNode*>(node);
        if (niNode != nullptr)
        {
            for (const auto& child : niNode->mChildren)
            {
                if (!child.empty())
                    processNode(child.getPtr(), worldTransform, output);
            }
        }
    }

    VulkanMesh MeshConverter::processGeometry(const Nif::NiGeometry* geom, const float worldTransform[16])
    {
        VulkanMesh mesh;
        std::memcpy(mesh.transform, worldTransform, 16 * sizeof(float));
        mesh.baseTexture = findBaseTexture(geom);
        mesh.alphaTested = findAlphaTested(geom);

        // Whether a specular highlight is allowed on this shape at all. Two gates, both taken from
        // nifosg::Loader::applyDrawableProperties:
        //
        //  - Morrowind-era files never get one. applyDrawableProperties zeroes the specular colour and
        //    the shininess outright for mVersion <= VER_MW, commented "While NetImmerse and Gamebryo
        //    support specular lighting, Morrowind has its support disabled". Vanilla content does ship
        //    non-black mSpecular in places; the OSG path throws it away and so must this one, or every
        //    rock in the game picks up a sheen the reference renderer does not show.
        //  - Later files can still switch it off per-shape. specEnabled starts true there and is only
        //    ever cleared from a property, so an absent NiSpecularProperty means enabled.
        //
        // BSShaderPPLightingProperty and BSLightingShaderProperty also drive specEnabled upstream. They
        // are not consulted here because the Vulkan path does not read Bethesda shader properties yet,
        // and leaving them out only ever errs towards less specular.
        const auto* specularProperty = findProperty<Nif::NiSpecularProperty>(geom);
        const bool specularEnabled
            = mNifVersion > Nif::NIFFile::VER_MW && (specularProperty == nullptr || specularProperty->mEnable);

        if (const auto* material = findProperty<Nif::NiMaterialProperty>(geom))
        {
            // Phong exponent to linear roughness: roughness = sqrt(2 / (glossiness + 2)), the standard
            // inversion of the Blinn-Phong normalisation term (Karis, "Physically Based Shading in
            // Mobile", SIGGRAPH 2013). Glossiness is clamped to [0, 128] first, the same ceiling
            // applyDrawableProperties applies before handing it to osg::Material::setShininess, because
            // NIFs do ship exponents far above what was ever renderable. That range maps to roughness
            // [0.124, 1.0]; the trailing clamp only guards against a garbage exponent in a broken file.
            //
            // Note this is deliberately *not* gated on specularEnabled even though OSG also forces
            // shininess to 0 there. Roughness widens the diffuse lobe too, and zeroing it for every
            // Morrowind shape would put the plastic look straight back. Suppressing the highlight is
            // specularStrength's job below.
            const float glossiness = std::clamp(material->mGlossiness, 0.f, 128.f);
            mesh.roughness = std::clamp(std::sqrt(2.f / (glossiness + 2.f)), 0.05f, 1.f);

            // A NiMaterialProperty has no scalar specular strength -- OSG feeds the colour straight to
            // osg::Material::setSpecular -- so its luminance stands in for one. Most Morrowind materials
            // store black here and would land on 0 even without the gate above.
            if (specularEnabled)
                mesh.specularStrength = std::clamp(luminance(material->mSpecular), 0.f, 1.f);

            // mEmissive scaled by mEmissiveMult, collapsed the same way. mEmissiveMult is only read for
            // Bethesda version 22 and above and defaults to 1, so Morrowind files pass the emissive
            // colour through unscaled. Not clamped to 1: emissiveMult is an HDR multiplier upstream and
            // the consumer should decide the exposure. Nothing consumes it yet.
            mesh.emissiveStrength = std::max(luminance(material->mEmissive) * material->mEmissiveMult, 0.f);
        }

        const Nif::NiGeometryData* data = geom->mData.getPtr();

        // Build index list
        std::vector<uint32_t> indices;

        if (data->mRecordType == Nif::RC_NiTriShapeData)
        {
            const auto* triData = static_cast<const Nif::NiTriShapeData*>(data);
            indices.reserve(triData->mTriangles.size());
            for (unsigned short idx : triData->mTriangles)
                indices.push_back(static_cast<uint32_t>(idx));
        }
        else if (data->mRecordType == Nif::RC_NiTriStripsData)
        {
            const auto* stripData = static_cast<const Nif::NiTriStripsData*>(data);
            indices = convertStripsToTriangles(stripData->mStrips);
        }

        if (indices.empty())
            return mesh;

        const uint32_t numVertices = static_cast<uint32_t>(data->mVertices.size());
        if (numVertices == 0)
            return mesh;

        // Interleaved vertex layout, declared once in vkgeometry.hpp because the terrain path builds
        // the same layout.
        constexpr size_t vertexStride = Vk::sVertexStride;

        const bool hasNormals = !data->mNormals.empty();
        const bool hasUVs = !data->mUVList.empty() && !data->mUVList[0].empty();
        const bool hasColors = !data->mColors.empty();

        std::vector<float> vertexData(numVertices * (vertexStride / sizeof(float)));

        // Seeded from vertex 0 rather than from +/-infinity so that a box always encloses real geometry:
        // with float limits, a mesh that never entered the loop would come out inverted and read as
        // "everything is inside". numVertices is known non-zero by here.
        for (int axis = 0; axis < 3; ++axis)
        {
            mesh.boundsMin[axis] = data->mVertices[0][axis];
            mesh.boundsMax[axis] = data->mVertices[0][axis];
        }

        for (uint32_t i = 0; i < numVertices; ++i)
        {
            float* dst = vertexData.data() + i * 12; // 12 floats per vertex (48 / 4)

            // Position
            const osg::Vec3f& pos = data->mVertices[i];
            dst[0] = pos.x();
            dst[1] = pos.y();
            dst[2] = pos.z();

            // Accumulated from dst rather than from pos so the bounds cannot drift out of the space the
            // vertex buffer is actually in, should a transform ever be folded in above.
            for (int axis = 0; axis < 3; ++axis)
            {
                mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], dst[axis]);
                mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], dst[axis]);
            }

            // Normal
            if (hasNormals)
            {
                const osg::Vec3f& n = data->mNormals[i];
                dst[3] = n.x();
                dst[4] = n.y();
                dst[5] = n.z();
            }
            else
            {
                dst[3] = 0.f;
                dst[4] = 1.f;
                dst[5] = 0.f;
            }

            // Texcoord
            if (hasUVs)
            {
                const osg::Vec2f& uv = data->mUVList[0][i];
                dst[6] = uv.x();
                dst[7] = uv.y();
            }
            else
            {
                dst[6] = 0.f;
                dst[7] = 0.f;
            }

            // Color, decoded to linear on the way in.
            //
            // NIF vertex colours are authored in gamma space like every other colour in Morrowind's
            // content, and gbuffer.frag multiplies them into an albedo that is already linear because
            // the textures are uploaded as _SRGB block formats. Leaving them encoded mixes two spaces
            // in one product. Alpha is a coverage scalar, not a colour, so it passes through.
            if (hasColors)
            {
                const osg::Vec4f& c = data->mColors[i];
                dst[8] = Vk::srgbToLinear(c.x());
                dst[9] = Vk::srgbToLinear(c.y());
                dst[10] = Vk::srgbToLinear(c.z());
                dst[11] = c.w();
            }
            else
            {
                dst[8] = 1.f;
                dst[9] = 1.f;
                dst[10] = 1.f;
                dst[11] = 1.f;
            }
        }

        Vk::Geometry geometry = Vk::uploadGeometry(mDevice, mCommandPool, vertexData.data(), numVertices,
            indices.data(), static_cast<uint32_t>(indices.size()), mesh.alphaTested);

        mesh.vertexBuffer = std::move(geometry.vertexBuffer);
        mesh.indexBuffer = std::move(geometry.indexBuffer);
        mesh.blas = std::move(geometry.blas);
        mesh.vertexCount = geometry.vertexCount;
        mesh.indexCount = geometry.indexCount;

        return mesh;
    }

}
