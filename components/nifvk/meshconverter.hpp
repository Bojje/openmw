#ifndef OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP
#define OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP

#include <cstdint>
#include <memory>
#include <string>
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

        // Raw base texture path from the NIF's NiTexturingProperty, empty if the shape is untextured.
        // Still needs Misc::ResourceHelpers::correctTexturePath() applied before use: Morrowind NIFs
        // reference textures without the "textures/" prefix and often name a .tga that ships as .dds.
        std::string baseTexture;

        // True when the shape's silhouette lives in its texture's alpha channel rather than in its
        // triangles. The ray tracer must leave such geometry non-opaque in the BLAS so the any-hit
        // shader runs and can discard the cut-out texels; flagged opaque, a leaf billboard casts the
        // shadow of a solid rectangle.
        bool alphaTested = false;
    };

    class MeshConverter
    {
    public:
        MeshConverter(Vk::Device& device, Vk::CommandPool& commandPool);

        std::vector<VulkanMesh> convert(const Nif::FileView& nif);

    private:
        void processNode(
            const Nif::NiAVObject* node, const float parentTransform[16], std::vector<VulkanMesh>& output);

        VulkanMesh processGeometry(const Nif::NiGeometry* geom, const float worldTransform[16]);

        Vk::Device& mDevice;
        Vk::CommandPool& mCommandPool;
    };

}

#endif
