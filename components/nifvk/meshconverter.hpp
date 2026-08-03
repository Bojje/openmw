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

namespace NifVk
{

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
        bool alphaTested = false;

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
