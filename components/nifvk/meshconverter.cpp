#include "meshconverter.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/nif/base.hpp>
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

    // Name of the node whose controller chain scrolls this shape's UVs, or empty if nothing does.
    //
    // Walks upward for the same reason findProperty does: in_lava_1024 hangs the controller off the
    // NiTriShape itself, while several of the magic effect shells hang it off the NiNode above.
    //
    // The isActive() gate mirrors nifloader.cpp's. A controller with the flag clear is never
    // instantiated there, so trusting it here would leave the renderer waiting every frame for a
    // TexMat that nothing creates.
    std::string findUvControllerNode(const Nif::NiAVObject* node)
    {
        for (const Nif::NiAVObject* cur = node; cur != nullptr;
             cur = cur->mParents.empty() ? nullptr : static_cast<const Nif::NiAVObject*>(cur->mParents.front()))
        {
            for (const Nif::NiTimeController* ctrl = cur->mController.getPtr(); ctrl != nullptr;
                 ctrl = ctrl->mNext.getPtr())
            {
                if (ctrl->mRecordType == Nif::RC_NiUVController && ctrl->isActive())
                    return cur->mName;
            }
        }
        return {};
    }

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

    // How this shape was authored to be composited, out of the two properties that say so.
    //
    // Both are inherited and the deepest wins, which is what findProperty already implements -- see
    // the note there for why searching nearest-first picks the same property nifosg's
    // applyDrawableProperties would end up with after walking the whole list.
    //
    // This replaces a findAlphaTested that answered one bool -- "is there a NiAlphaProperty at all"
    // -- and then threw the flags away. That was enough to keep the ray tracer honest and not enough
    // for anything else: it could not tell a shape authored to blend from one authored to cut out,
    // and it could not tell what threshold a cut-out wanted, so gbuffer.frag decided for every shape
    // in the game with one hardcoded 0.5.
    //
    // Known gap, unchanged and still worth writing down: Morrowind ships some cut-out foliage with no
    // NiAlphaProperty at all, relying on the texture's alpha alone. The signal that would catch it is
    // the pixel format -- most diffuse maps are BC1_RGB and sample a == 1 -- but the texture is not
    // resolved until after conversion has built the BLAS, so it cannot reach here. The flat 0.5
    // cutout gbuffer.frag keeps as its default is what covers those files, which is one more reason
    // that default is not safe to remove.
    NifVk::MeshRenderState findRenderState(const Nif::NiAVObject* node)
    {
        NifVk::MeshRenderState state;

        if (const auto* alpha = findProperty<Nif::NiAlphaProperty>(node))
        {
            state.alphaTest = alpha->useAlphaTesting();
            state.alphaFunc = static_cast<uint8_t>(alpha->alphaTestMode());
            state.alphaThreshold = alpha->mThreshold;
            state.blend = alpha->useAlphaBlending();
            state.srcFactor = static_cast<uint8_t>(alpha->sourceBlendMode());
            state.dstFactor = static_cast<uint8_t>(alpha->destinationBlendMode());
        }

        // mDrawMode only, not mEnabled. Those are two unrelated things on one record: mEnabled turns
        // the stencil *buffer* on, which this renderer has none of, while the draw mode says which
        // faces to rasterise and is honoured whether or not stencilling is. nifosg sets the cull mode
        // outside its own `if (stencilprop->mEnabled)` for exactly that reason
        // (nifloader.cpp:2504-2511), and reading mEnabled here would silently drop the majority of
        // two-sided shapes.
        if (const auto* stencil = findProperty<Nif::NiStencilProperty>(node))
            state.twoSided = stencil->mDrawMode == Nif::NiStencilProperty::DrawMode::Both;

        return state;
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

    void MeshConverter::describeSkin(const Nif::NiGeometry* geom, VulkanMesh& mesh)
    {
        Vk::identityMat4(mesh.skinInvBind);

        const auto* skin = static_cast<const Nif::NiSkinInstance*>(geom->mSkin.getPtr());
        if (skin == nullptr || skin->mData.empty())
            return;

        const Nif::NiSkinData* data = skin->mData.getPtr();
        if (data == nullptr || data->mBones.empty())
            return;

        // Heaviest bone by total weight, not the first: a body part's bone list is in file order and
        // the first entry is frequently a parent that barely touches it.
        size_t dominant = 0;
        float bestWeight = -1.0f;
        for (size_t i = 0; i < data->mBones.size() && i < skin->mBones.size(); ++i)
        {
            float total = 0.0f;
            for (const auto& [vertex, weight] : data->mBones[i].mWeights)
            {
                (void)vertex;
                total += weight;
            }
            if (total > bestWeight)
            {
                bestWeight = total;
                dominant = i;
            }
        }

        if (dominant >= skin->mBones.size() || skin->mBones[dominant].empty())
            return;

        const Nif::NiAVObject* bone = skin->mBones[dominant].getPtr();
        if (bone == nullptr || bone->mName.empty())
            return;

        mesh.skinBone = bone->mName;
        nifTransformToMat4(data->mBones[dominant].mTransform, mesh.skinInvBind);

        collectSkinWeights(skin, data, mesh);
    }

    void MeshConverter::collectSkinWeights(
        const Nif::NiSkinInstance* skin, const Nif::NiSkinData* data, VulkanMesh& mesh)
    {
        const size_t boneCount = std::min(data->mBones.size(), skin->mBones.size());
        if (boneCount == 0 || mesh.vertexCount == 0)
            return;

        // The NIF stores this the other way round from what a vertex shader wants: a list per bone of
        // the vertices it touches, rather than a list per vertex of the bones that touch it. Inverting
        // it is the whole of this function.
        //
        // A bone index in the output indexes mesh.skinBones, not the NIF's list. They differ whenever a
        // bone is skipped -- an unnamed one, or one past the 255 an eight-bit index can name -- and the
        // renderer only ever sees the compacted list.
        struct Influence
        {
            uint8_t bone;
            float weight;
        };
        std::vector<std::vector<Influence>> perVertex(mesh.vertexCount);

        mesh.skinBones.reserve(boneCount);
        mesh.skinInvBinds.reserve(boneCount);

        for (size_t i = 0; i < boneCount; ++i)
        {
            if (skin->mBones[i].empty())
                continue;
            const Nif::NiAVObject* boneNode = skin->mBones[i].getPtr();
            if (boneNode == nullptr || boneNode->mName.empty())
                continue;
            if (mesh.skinBones.size() >= sMaxSkinBones)
            {
                Log(Debug::Warning) << "Vulkan: skinned shape has more than " << sMaxSkinBones
                                    << " bones, the rest are ignored";
                break;
            }

            const uint8_t compacted = static_cast<uint8_t>(mesh.skinBones.size());
            mesh.skinBones.emplace_back(boneNode->mName);

            std::array<float, 16> invBind;
            nifTransformToMat4(data->mBones[i].mTransform, invBind.data());
            mesh.skinInvBinds.push_back(invBind);

            for (const auto& [vertex, weight] : data->mBones[i].mWeights)
            {
                if (vertex >= mesh.vertexCount || weight <= 0.0f)
                    continue;
                perVertex[vertex].push_back({ compacted, weight });
            }
        }

        if (mesh.skinBones.empty())
            return;

        // Four influences per vertex, which is what the shader blends and what every skinning
        // implementation of this era settled on. Morrowind's own parts rarely exceed two.
        mesh.skinAttributes.assign(static_cast<size_t>(mesh.vertexCount) * sSkinAttributeStride, 0);

        uint32_t unweighted = 0;

        for (uint32_t v = 0; v < mesh.vertexCount; ++v)
        {
            auto& influences = perVertex[v];
            if (influences.empty())
            {
                ++unweighted;
                continue;
            }

            if (influences.size() > 4)
            {
                std::partial_sort(influences.begin(), influences.begin() + 4, influences.end(),
                    [](const Influence& a, const Influence& b) { return a.weight > b.weight; });
                influences.resize(4);
            }

            float total = 0.0f;
            for (const Influence& influence : influences)
                total += influence.weight;
            if (total <= 0.0f)
                continue;

            uint8_t* dst = mesh.skinAttributes.data() + static_cast<size_t>(v) * sSkinAttributeStride;


            // Renormalised over the four that were kept, and quantised to eight bits per weight.
            // The quantisation is why the last weight is the remainder rather than its own rounding:
            // four independently rounded weights need not sum to 255, and a vertex whose weights sum
            // to slightly less than one shrinks towards the origin visibly on a limb.
            unsigned int assigned = 0;
            for (size_t i = 0; i < influences.size(); ++i)
            {
                dst[i] = influences[i].bone;
                if (i + 1 == influences.size())
                {
                    dst[4 + i] = static_cast<uint8_t>(255u - assigned);
                }
                else
                {
                    const unsigned int quantised = static_cast<unsigned int>(
                        std::lround(influences[i].weight / total * 255.0f));
                    const uint8_t clamped = static_cast<uint8_t>(std::min(quantised, 255u - assigned));
                    dst[4 + i] = clamped;
                    assigned += clamped;
                }
            }
        }

        // A vertex the NIF gave no influences at all does not collapse -- gbuffer_skinned.vert falls
        // back to identity for it -- but identity means it stays in *bind pose* while every vertex
        // around it is posed by the animation. On a garment that is a torn seam: the sleeve sits where
        // the modeller left it while the torso moves, and the body underneath shows through the gap.
        //
        // It is silent by construction, so it needs saying out loud. The two ways to get here are a
        // bone reference this loop skipped -- unnamed, or empty -- which takes every vertex weighted
        // only to it with it, and mBones lists of unequal length between NiSkinData and NiSkinInstance,
        // which drops the tail of the longer one.
        if (unweighted > 0)
        {
            Log(Debug::Warning) << "Vulkan: skinned shape on bone \"" << mesh.skinBone << "\" has "
                                << unweighted << " of " << mesh.vertexCount
                                << " vertices with no bone influence; they stay in bind pose. "
                                << data->mBones.size() << " skin data bones, " << skin->mBones.size()
                                << " instance bones, " << mesh.skinBones.size() << " kept";
        }
    }

    void MeshConverter::uploadSkin(VulkanMesh& mesh)
    {
        if (mesh.skinAttributes.empty())
            return;

        // Vertex buffer usage only. Nothing traces against a skinned shape -- actors are rasterised
        // and kept out of the acceleration structure -- so this needs neither a device address nor
        // the acceleration-structure build flag the position buffer carries.
        mesh.skinBuffer = std::make_unique<Vk::Buffer>(Vk::Buffer::createWithStaging(mDevice, mCommandPool,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, mesh.skinAttributes.data(),
            static_cast<VkDeviceSize>(mesh.skinAttributes.size())));

        // The CPU copy has done its job. A body's worth of parts is a few hundred kilobytes of it and
        // nothing reads it again.
        mesh.skinAttributes.clear();
        mesh.skinAttributes.shrink_to_fit();
    }

    std::unordered_map<std::string, std::array<float, 16>> MeshConverter::collectNodeTransforms(
        const Nif::FileView& nif)
    {
        std::unordered_map<std::string, std::array<float, 16>> transforms;

        float identity[16];
        Vk::identityMat4(identity);

        for (size_t i = 0; i < nif.numRoots(); ++i)
        {
            const Nif::Record* root = nif.getRoot(i);
            if (root == nullptr)
                continue;

            const auto* avObject = dynamic_cast<const Nif::NiAVObject*>(root);
            if (avObject != nullptr)
                collectNodeTransformsRecursive(avObject, identity, transforms);
        }

        return transforms;
    }

    void MeshConverter::collectNodeTransformsRecursive(const Nif::NiAVObject* node, const float parentTransform[16],
        std::unordered_map<std::string, std::array<float, 16>>& output)
    {
        if (node == nullptr)
            return;

        // isHidden() is deliberately not checked, unlike in processNode. A hidden node still positions
        // its children, and several of Morrowind's skeletons hide bones that body parts hang off.

        float localMat[16];
        nifTransformToMat4(node->mTransform, localMat);

        float worldTransform[16];
        Vk::multiplyMat4(parentTransform, localMat, worldTransform);

        if (!node->mName.empty())
        {
            // First writer wins. Morrowind's skeletons repeat a few names -- most visibly "Bip01" on
            // both the root and an accessory node -- and the one nearest the root is the one body parts
            // are meant to hang from.
            std::array<float, 16> stored;
            std::copy(std::begin(worldTransform), std::end(worldTransform), stored.begin());
            output.emplace(node->mName, stored);
        }

        const auto* niNode = dynamic_cast<const Nif::NiNode*>(node);
        if (niNode != nullptr)
        {
            for (const auto& child : niNode->mChildren)
            {
                if (!child.empty())
                    collectNodeTransformsRecursive(child.getPtr(), worldTransform, output);
            }
        }
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
            // Morrowind's own baked blob shadows, dropped by name the way NifOsg drops them
            // (nifloader.cpp:851-858). They are flat black quads at the foot of a model with the blob
            // in a texture's alpha, meant to be blended; the G-buffer has no blending and only the
            // hard cutout in gbuffer.frag, so what actually rasterises is the opaque middle of the
            // blob, black, writing depth. Left in, every object that is ever re-oriented drags a
            // hard-edged black patch up out of the ground and into its own silhouette.
            //
            // Only for Morrowind-era files, which is the same gate NifOsg uses. Later games name real
            // geometry this way and deleting a shape on its name alone would take something visible
            // with it.
            if (mNifVersion <= Nif::NIFFile::VER_MW
                && (Misc::StringUtils::ciStartsWith(node->mName, "shadow")
                    || Misc::StringUtils::ciStartsWith(node->mName, "tri shadow")))
                return;

            const auto* geom = static_cast<const Nif::NiGeometry*>(node);
            if (!geom->mData.empty())
            {
                VulkanMesh mesh = processGeometry(geom, worldTransform);
                mesh.skinned = !geom->mSkin.empty();
                if (mesh.skinned)
                {
                    describeSkin(geom, mesh);
                    uploadSkin(mesh);
                }
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
        mesh.uvControllerNode = findUvControllerNode(geom);
        mesh.baseTexture = findBaseTexture(geom);
        mesh.renderState = findRenderState(geom);
        // What the BLAS needs from all of that: whether a ray may treat this surface as solid. A
        // shape that tests or blends has a silhouette its triangles do not describe, either way.
        mesh.alphaTested = mesh.renderState.alphaTest || mesh.renderState.blend;

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
