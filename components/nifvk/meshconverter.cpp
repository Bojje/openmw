#include "meshconverter.hpp"

#include <algorithm>
#include <cstring>

#include <components/debug/debuglog.hpp>
#include <components/nif/data.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/vk/vkdevice.hpp>

namespace
{

    void identityMat4(float out[16])
    {
        std::memset(out, 0, 16 * sizeof(float));
        out[0] = 1.f;
        out[5] = 1.f;
        out[10] = 1.f;
        out[15] = 1.f;
    }

    // Column-major 4x4 matrix multiply: out = a * b
    // Column-major: element (row, col) is at index [col * 4 + row]
    void multiplyMat4(const float a[16], const float b[16], float out[16])
    {
        float tmp[16];
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                tmp[col * 4 + row] = sum;
            }
        }
        std::memcpy(out, tmp, 16 * sizeof(float));
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

        float identity[16];
        identityMat4(identity);

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
        multiplyMat4(parentTransform, localMat, worldTransform);

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

        // Interleaved vertex layout (48 bytes per vertex):
        //   position: vec3  (offset 0,  12 bytes)
        //   normal:   vec3  (offset 12, 12 bytes)
        //   texcoord: vec2  (offset 24,  8 bytes)
        //   color:    vec4  (offset 32, 16 bytes)
        constexpr size_t vertexStride = 48;

        const bool hasNormals = !data->mNormals.empty();
        const bool hasUVs = !data->mUVList.empty() && !data->mUVList[0].empty();
        const bool hasColors = !data->mColors.empty();

        std::vector<float> vertexData(numVertices * (vertexStride / sizeof(float)));

        for (uint32_t i = 0; i < numVertices; ++i)
        {
            float* dst = vertexData.data() + i * 12; // 12 floats per vertex (48 / 4)

            // Position
            const osg::Vec3f& pos = data->mVertices[i];
            dst[0] = pos.x();
            dst[1] = pos.y();
            dst[2] = pos.z();

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

            // Color
            if (hasColors)
            {
                const osg::Vec4f& c = data->mColors[i];
                dst[8] = c.x();
                dst[9] = c.y();
                dst[10] = c.z();
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

        mesh.vertexCount = numVertices;
        mesh.indexCount = static_cast<uint32_t>(indices.size());

        // Create GPU buffers via staging
        const VkDeviceSize vertexSize = numVertices * vertexStride;
        const VkDeviceSize indexSize = indices.size() * sizeof(uint32_t);

        VkBufferUsageFlags vertexFlags = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        VkBufferUsageFlags indexFlags = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (mDevice.rayTracingSupported())
        {
            vertexFlags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
            indexFlags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        }

        auto vb = Vk::Buffer::createWithStaging(mDevice, mCommandPool,
            vertexFlags, vertexData.data(), vertexSize);
        mesh.vertexBuffer = std::make_unique<Vk::Buffer>(std::move(vb));

        auto ib = Vk::Buffer::createWithStaging(mDevice, mCommandPool,
            indexFlags, indices.data(), indexSize);
        mesh.indexBuffer = std::make_unique<Vk::Buffer>(std::move(ib));

        // Build BLAS for ray tracing
        if (mDevice.rayTracingSupported())
        {
            auto blas = Vk::AccelerationStructure::createBLAS(mDevice, mCommandPool, *mesh.vertexBuffer,
                mesh.vertexCount, vertexStride, VK_FORMAT_R32G32B32_SFLOAT, *mesh.indexBuffer, mesh.indexCount,
                VK_INDEX_TYPE_UINT32);
            mesh.blas = std::make_unique<Vk::AccelerationStructure>(std::move(blas));
        }

        return mesh;
    }

}
