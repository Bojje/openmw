#ifndef OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP
#define OPENMW_COMPONENTS_NIFVK_MESHCONVERTER_HPP

#include <cstdint>
#include <memory>
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
