#ifndef OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP
#define OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <components/vk/vkbuffer.hpp>
#include <components/vk/vkraytracing.hpp>

namespace Nif
{
    class FileView;
    struct NiAVObject;
    struct NiGeometry;
}

namespace Vk
{
    class Device;
    class CommandPool;
}

namespace Nif
{
    struct NiSkinInstance;
    struct NiSkinData;
}

namespace NifVk
{

    /// Bytes of skin attribute per vertex: four bone indices then four quantised weights.
    inline constexpr size_t sSkinAttributeStride = 8;

    /// How many distinct bones one shape may name. Eight bits an index, and index 255 is left alone
    /// so nothing has to distinguish "bone 255" from a padding byte.
    inline constexpr size_t sMaxSkinBones = 255;

    /// glBlendFunc factor codes in the order NiAlphaProperty stores them (property.hpp:432-444).
    /// Only the three that shipped content actually uses are named; the rest are passed through as
    /// numbers because nothing here needs to understand them.
    inline constexpr uint8_t sBlendOne = 0;
    inline constexpr uint8_t sBlendSrcAlpha = 6;
    inline constexpr uint8_t sBlendInvSrcAlpha = 7;

    /// How a shape was authored to be composited: its NiAlphaProperty and NiStencilProperty, decoded
    /// but not yet judged.
    ///
    /// Kept as authored rather than as a decision, because the three consumers make different ones --
    /// the raster pass, the ray tracer and the forward additive pass each look at a different part of
    /// this. Counted out of Morrowind.bsa, Tribunal.bsa and Bloodmoon.bsa together, 4602 records over
    /// 7320 files, so the notes below are what the content contains rather than what the format allows.
    struct MeshRenderState
    {
        /// Alpha testing, NiAlphaProperty bit 9.
        ///
        /// Vanilla Morrowind never sets this. Not rarely -- all 3099 of its records are blend-only
        /// with a threshold of zero, because Morrowind relied on sorted blending and the D3D alpha
        /// test was left off. The expansions do set it, 1140 records across 126 files, and those are
        /// the only files whose silhouette the renderer can get right by reading the flag.
        bool alphaTest = false;

        /// The comparison, in the glAlphaFunc order NiAlphaProperty::alphaTestMode returns and
        /// nifosg's getTestMode switches on (nifloader.cpp:1925-1949): 0 ALWAYS, 1 LESS, 2 EQUAL,
        /// 3 LEQUAL, 4 GREATER, 5 NOTEQUAL, 6 GEQUAL, 7 NEVER. The fragment is kept where
        /// `alpha FUNC threshold` holds. Shipped content uses GEQUAL (1007) and GREATER (133).
        uint8_t alphaFunc = 0;

        /// NiAlphaProperty::mThreshold, still in 0..255 -- divided by 255 wherever it is compared,
        /// which is what nifosg::handleAlphaTesting does too (nifloader.cpp:2319). Shipped values are
        /// 192 and 100; neither is the 0.5 the G-buffer currently assumes.
        uint8_t alphaThreshold = 0;

        /// Alpha blending, NiAlphaProperty bit 0. True for every record in all three archives.
        bool blend = false;

        /// Source and destination factors, in the glBlendFunc order the NiAlphaProperty accessors
        /// return. Shipped content uses exactly three pairs: SRC_ALPHA/INV_SRC_ALPHA 3808 times,
        /// SRC_ALPHA/ONE 786 times, and ONE/ONE once.
        uint8_t srcFactor = sBlendSrcAlpha;
        uint8_t dstFactor = sBlendInvSrcAlpha;

        /// NiStencilProperty::DrawMode::Both, which nifosg honours by switching GL_CULL_FACE off
        /// (nifloader.cpp:2506-2507) while leaving the winding alone.
        ///
        /// No shipped file uses it. The string "NiStencilProperty" does not appear once in
        /// Morrowind.bsa, Tribunal.bsa or Bloodmoon.bsa, where NiTexturingProperty appears 25,797
        /// times in the same scan -- so that is absence, not a parser that missed it. This is here
        /// for mods, which do author it, and it is why the two-sided path is deliberately the cheap
        /// one rather than the good one.
        bool twoSided = false;

        /// Whether this shape adds light rather than covering what is behind it.
        ///
        /// This is the one blend mode a deferred renderer can honour cheaply, and the reason is that
        /// a destination factor of GL_ONE makes the draw order irrelevant -- addition commutes, so
        /// there is nothing to sort -- and the surface emits rather than receives, so there is no
        /// lighting to redo. Everything else needs both, plus ray traced shadow and indirect terms
        /// that only exist for the opaque surface behind it.
        bool additive() const { return blend && dstFactor == sBlendOne; }
    };

    struct VulkanMesh
    {
        std::unique_ptr<Vk::Buffer> vertexBuffer;
        std::unique_ptr<Vk::Buffer> indexBuffer;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        std::unique_ptr<Vk::AccelerationStructure> blas;

        // Transform from NIF node hierarchy, column-major 4x4
        float transform[16];

        // Axis-aligned bounds of the vertex buffer's contents, in the same object space as the
        // positions themselves -- i.e. before `transform` and before the per-instance matrix. Exists so
        // the renderer can frustum-cull instances instead of recording a draw for every one in the
        // active cell grid; to test a box, push the eight corners through the instance matrix and
        // re-fit, or use the standard transformed-AABB trick (centre through the matrix, extent through
        // its absolute value). A zero extent on an axis is legitimate and common -- foliage billboards
        // are flat -- so a culling test must use >= / <=, not a strict comparison.
        float boundsMin[3] = { 0.0f, 0.0f, 0.0f };
        float boundsMax[3] = { 0.0f, 0.0f, 0.0f };

        // Raw base texture path from the NIF's NiTexturingProperty, empty if the shape is untextured.
        // Still needs Misc::ResourceHelpers::correctTexturePath() applied before use: Morrowind NIFs
        // reference textures without the "textures/" prefix and often name a .tga that ships as .dds.
        std::string baseTexture;

        // True when the shape's silhouette lives in its texture's alpha channel rather than in its
        // triangles. The ray tracer must leave such geometry non-opaque in the BLAS so the any-hit
        // shader runs and can discard the cut-out texels; flagged opaque, a leaf billboard casts the
        // shadow of a solid rectangle.
        //
        // Derived from renderState below rather than read separately: it is true when the shape
        // either tests or blends. That is a hair narrower than the old rule, which was "a
        // NiAlphaProperty exists at all" -- a record with neither bit set would now be treated as
        // solid. No such record exists in any of the three shipped archives, and a property that
        // asks for neither test nor blend is asking for nothing.
        bool alphaTested = false;

        // How the shape was authored to be composited. See MeshRenderState; the short version is
        // that the renderer honours the authored alpha test where there is one, draws additive
        // shapes forward instead of deferring them, and turns culling off for two-sided ones.
        MeshRenderState renderState;

        // True when the shape carries a NiSkinInstance. It is converted in bind pose either way, but
        // the distinction matters to whoever places it: a skinned shape's vertices are already in the
        // skeleton's space, while a rigid one's are in the space of the node that holds it. Hanging a
        // skinned part off a bone transform as well applies that bone twice, which throws the part
        // across the room -- visibly, and only for some parts, which reads as a bad NIF.
        bool skinned = false;

        // For a skinned shape: the name of the bone that carries the most of its weight, and that
        // bone's inverse bind transform from the NiSkinData. Placing the shape at
        // boneWorld * invBind puts it where it sits in the skeleton's bind pose, which is the best a
        // renderer with no skinning can do. Empty for a rigid shape.
        //
        // The dominant bone rather than all of them: a hand or a foot is skinned to one bone and this
        // is then exact, while a chest spans several and this places it as though it were rigid to its
        // heaviest. Real skinning is a per-vertex weighted sum and is a different piece of work.
        std::string skinBone;
        float skinInvBind[16];

        // The full skin, for a renderer that can blend it. Empty for a rigid shape, and also empty
        // for a skinned one whose bones are all unnamed, which is what the dominant-bone fields above
        // remain the fallback for.
        //
        // skinBones and skinInvBinds are parallel: entry i is a bone's name and the transform that
        // takes a vertex from the skeleton's bind pose into that bone's space. A vertex is posed as
        // sum over its influences of weight * boneWorld[i] * skinInvBinds[i] * vertex.
        //
        // The names are what tie this to a live skeleton. A part file names bones it does not
        // contain; the caller looks each one up in the actor's skeleton, and a name that is not
        // found has to be treated as identity rather than skipped, or the part collapses.
        std::vector<std::string> skinBones;
        std::vector<std::array<float, 16>> skinInvBinds;

        // Two bytes per influence, four influences per vertex, in vertex order: bone indices into
        // skinBones first, then weights quantised to eight bits and summing to 255. A vertex with
        // fewer than four influences has zeroes in the rest, and a zero weight makes the matching
        // index harmless, so the shader needs no count.
        //
        // Kept as bytes rather than floats because this is a second vertex buffer bound alongside the
        // first, and eight bytes a vertex against the main buffer's forty-eight is worth the unpack.
        std::vector<uint8_t> skinAttributes;
        std::unique_ptr<Vk::Buffer> skinBuffer;

        // Surface response derived from the shape's NiMaterialProperty. All three are scalars because
        // the G-buffer has one channel each to spare; the NIF stores colours, which are collapsed by
        // luminance. A shape with no NiMaterialProperty keeps the defaults below.

        // Linear roughness derived from the material's Phong glossiness. 1.0 = fully rough, which is
        // both the NIF default (mGlossiness defaults to 0) and the right answer for the bulk of
        // Morrowind, where nothing has a specular highlight to sharpen.
        float roughness = 1.0f;

        // How strongly the surface reflects specularly, 0 = not at all. Zero unless the file both
        // postdates Morrowind and leaves specular switched on, so the common case is 0 -- see the
        // derivation in processGeometry for why that gate is deliberately strict.
        float specularStrength = 0.0f;

        // Luminance of the material's emissive colour times its emissive multiplier. Not consumed by
        // the renderer yet; gbuffer.frag still writes a constant emission.
        float emissiveStrength = 0.0f;

        // Name of the NIF node whose NiUVController scrolls this shape's texture, empty if none does.
        // Non-empty is what marks the shape as animated.
        //
        // The name, not the curve. The curve stays where it already is, in NifOsg::UVController, and
        // the renderer reads the osg::TexMat that evaluates from it every frame. Copying the keys
        // here instead would mean a second implementation of a cubic Hermite spline plus its phase,
        // its start/stop window and its extrapolation mode -- all four of which the shipped files
        // actually use. The day the two implementations disagree, lava flows at one speed in the
        // world and another in the ray traced reflection.
        //
        // The controller's node rather than the geometry's, because that is precisely where the
        // matrix turns up: NifOsg::Loader hangs a CompositeStateSetUpdater on the node holding the
        // controller. Recording the geometry's own name would fail for the files that put the
        // controller on the NiNode above the shape, and it would fail by finding no TexMat and
        // quietly not animating.
        std::string uvControllerNode;
    };

    class MeshConverter
    {
    public:
        MeshConverter(Vk::Device& device, Vk::CommandPool& commandPool);

        std::vector<VulkanMesh> convert(const Nif::FileView& nif);

        /// Every named node in \a nif with its accumulated transform from the file's root, which for a
        /// skeleton file is the bind pose. Column-major, the same convention as VulkanMesh::transform.
        ///
        /// Needed because an NPC is not one model. Its own model is meshes/base_anim.nif, a skeleton
        /// with no geometry at all, and the body is a dozen separate part files each of which is
        /// authored around the origin and expects to be hung on a bone by name. Converted on its own a
        /// part lands at the actor's feet, so a whole NPC converted naively is a heap.
        ///
        /// Static, and takes no device: this reads structure, uploads nothing, and the caller is
        /// expected to do it once per skeleton and cache the result.
        static std::unordered_map<std::string, std::array<float, 16>> collectNodeTransforms(const Nif::FileView& nif);

    private:
        static void collectNodeTransformsRecursive(const Nif::NiAVObject* node, const float parentTransform[16],
            std::unordered_map<std::string, std::array<float, 16>>& output);

        // Fills in skinBone and skinInvBind for a shape that carries a NiSkinInstance.
        static void describeSkin(const Nif::NiGeometry* geom, VulkanMesh& mesh);

        // Inverts the NIF's per-bone weight lists into the per-vertex form a vertex shader wants,
        // and fills skinBones, skinInvBinds and skinAttributes.
        static void collectSkinWeights(
            const Nif::NiSkinInstance* skin, const Nif::NiSkinData* data, VulkanMesh& mesh);

        // Moves skinAttributes onto the device and frees the CPU copy.
        void uploadSkin(VulkanMesh& mesh);

        void processNode(
            const Nif::NiAVObject* node, const float parentTransform[16], std::vector<VulkanMesh>& output);

        VulkanMesh processGeometry(const Nif::NiGeometry* geom, const float worldTransform[16]);

        Vk::Device& mDevice;
        Vk::CommandPool& mCommandPool;

        // NIF format version of the file currently being converted, set by convert(). Needed because
        // Morrowind-era files have specular lighting disabled wholesale and the records themselves
        // carry no hint of that -- only the file header does.
        std::uint32_t mNifVersion = 0;
    };

}

#endif
