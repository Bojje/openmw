#ifndef OPENMW_MWRENDER_VKSKYMESH_H
#define OPENMW_MWRENDER_VKSKYMESH_H

#ifdef OPENMW_USE_VULKAN

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.h>

namespace osg
{
    class Geometry;
}

namespace Vk
{
    class CommandPool;
    class Device;
}

namespace MWRender
{
    /// What one sky mesh is drawn from. An indexCount of 0 means there is nothing to draw, which is
    /// also the answer when the geometry could not be converted -- see SkyMeshCache::getOrUpload.
    struct SkyMeshBuffers
    {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
    };

    /// The sky's two NIF meshes -- the cloud layer and the night sky dome -- copied out of the live
    /// OSG graph onto the device, once each.
    ///
    /// Off the osg::Geometry rather than back through NifVk::MeshConverter, and the per-vertex alpha is
    /// why. ModVertexAlphaVisitor rewrites the colour array of every sky shape *after* the NIF is
    /// loaded and after Resource::SceneManager's optimizer has merged and reordered it -- 0 on cloud
    /// vertices 49-64, 0.25098 on 33-48, 1 elsewhere (skyutil.cpp lines 1061-1069), and for the stars a
    /// rule that reads the NIF's own colours (lines 1071-1080). Both shaders multiply by that alpha and
    /// it is the whole reason a cloud layer fades out at the horizon instead of ending in a hard line.
    /// Converting the NIF again would produce the colours the file was authored with, and the index
    /// ranges above only mean anything against the vertex order the optimizer left behind -- so
    /// re-deriving them is guessing at a number this graph already holds.
    ///
    /// The arrays are readable because nothing frees them: SceneUtil::CopyOp deep-copies nodes but not
    /// arrays (clone.cpp lines 20-25), so every instance shares one CPU-side copy owned by the template
    /// in Resource::SceneManager's cache, and OSG has no API that discards client array data after a
    /// VBO upload. Checked by reading; not verified against a running game.
    ///
    /// Uploaded once and kept. Only the transform, the texture, the opacity and the UV scroll change
    /// from frame to frame, and none of those touch a buffer.
    class SkyMeshCache
    {
    public:
        SkyMeshCache(Vk::Device& device, Vk::CommandPool& commandPool);
        ~SkyMeshCache();

        SkyMeshCache(const SkyMeshCache&) = delete;
        SkyMeshCache& operator=(const SkyMeshCache&) = delete;

        /// The buffers for \a geometry, uploading it on the first frame it is seen.
        ///
        /// Answers an empty SkyMeshBuffers for anything it cannot convert -- no vertex array, a vertex
        /// array that is not Vec3, or no triangles -- rather than throwing. A sky mesh a mod has
        /// replaced with something this does not understand should cost the clouds, not the process.
        SkyMeshBuffers getOrUpload(const osg::Geometry& geometry);

    private:
        struct Entry;

        Vk::Device& mDevice;
        Vk::CommandPool& mCommandPool;
        // Two meshes in a vanilla game, four drawables at most once both cloud layers are counted, so
        // a linear scan beats a hash of a pointer. Held by unique_ptr so that growing the vector does
        // not move a Vk::Geometry whose buffers a command buffer may still be referencing.
        std::vector<std::unique_ptr<Entry>> mEntries;
    };
}

#endif
#endif
