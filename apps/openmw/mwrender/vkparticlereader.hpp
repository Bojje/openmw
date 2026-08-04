#ifndef OPENMW_MWRENDER_VKPARTICLEREADER_H
#define OPENMW_MWRENDER_VKPARTICLEREADER_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <osg/Vec3f>

#include <components/vk/vkrenderer.hpp>

namespace osg
{
    class Node;
}

namespace MWRender
{
    /// Collects every live particle in the OSG scene graph into quads the Vulkan renderer can draw.
    ///
    /// This renderer does not simulate. OSG still owns the world, the animation and the physics, and
    /// particles are no different: emitters, gravity, colliders, colour over life and lifetimes are
    /// all driven by osgParticle every frame through the update traversal, which runs whichever
    /// backend is presenting. Reading that state is the same arrangement actors already use, where
    /// the pose comes from the live SceneUtil::Skeleton rather than from a second animation system.
    ///
    /// Simulating them here instead would make this the first place the two backends could disagree
    /// about the same world -- a different timestep, a different random sequence, different behaviour
    /// on pause and on a changed timescale -- and would give up the ability to check the result
    /// against the OSG image, which is the only reason any of this is measurable.
    ///
    /// Verified before it was written: an interior with twelve emitters reported 364 live particles
    /// and positions that moved every frame under gravity, with the Vulkan backend presenting.
    class ParticleReader
    {
    public:
        /// \a resolveTexture maps an image file name to its storage index in the caller's texture
        /// table, loading it if needed, so particle textures are cached, evicted and shared on the
        /// same terms as everything else.
        ///
        /// A storage index and not a sampler slot, which is the whole point. The slot is not settled
        /// until the caller's texture sync has run, and that is later in the frame than this reader
        /// runs; worse, the sync renumbers the array rather than appending to it, so a slot resolved
        /// before it names a different texture on every frame a cell unloads. The index is stable
        /// across all of that. See resolveTextureSlots for where it becomes a slot.
        explicit ParticleReader(std::function<std::size_t(const std::string&)> resolveTexture);

        /// Walks \a sceneRoot and refills the quad list. Cheap enough to do every frame: the walk is
        /// over the loaded cell graph and the particle count is in the hundreds.
        void collect(osg::Node* sceneRoot);

        /// Sorts the collected quads back to front from \a cameraPosition and rebuilds the runs.
        ///
        /// Separate from collect because they happen at different points in the frame: collecting has
        /// to precede the texture sync, and sorting needs the camera, which is not known until the
        /// frame is being rendered.
        void sortForCamera(const osg::Vec3f& cameraPosition);

        /// Turns every quad's textureIndex from the storage index collect() left in it into the
        /// sampler slot \a slotOf answers now. Call once per collect(), after the caller's texture
        /// sync and before the quads are handed to the renderer.
        ///
        /// Once, and that is a real constraint rather than a caution: a second pass would read a
        /// slot as though it were a storage index and resolve it again to something unrelated.
        ///
        /// In place rather than through a parallel array of indices, which is what the meshes use
        /// and what SkyReader can afford. sortForCamera reorders the quads, so a parallel array
        /// would have to be permuted along with them for no gain. The cost is that textureIndex
        /// means one thing between collect() and this call and another after it, which is why this
        /// is a step with a name rather than something folded quietly into the sort.
        void resolveTextureSlots(const std::function<uint32_t(std::size_t)>& slotOf);

        const std::vector<Vk::ParticleQuad>& quads() const { return mQuads; }
        const std::vector<Vk::ParticleRun>& runs() const { return mRuns; }

        /// Storage indices of every texture referenced this frame, for the caller's live set. Without
        /// this the eviction pass sees particle textures referenced by no mesh, no actor and no
        /// terrain chunk, decides they are dead, and replaces them with the white fallback -- so every
        /// flame in the game draws as a solid white quad.
        const std::vector<std::size_t>& textureIndices() const { return mTextureIndices; }

    private:
        std::function<std::size_t(const std::string&)> mResolveTexture;
        std::vector<Vk::ParticleQuad> mQuads;
        std::vector<Vk::ParticleRun> mRuns;
        std::vector<std::size_t> mTextureIndices;
    };
}

#endif
#endif
