#ifndef OPENMW_MWRENDER_VKPARTICLEREADER_H
#define OPENMW_MWRENDER_VKPARTICLEREADER_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

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
        /// \a resolveTexture maps an image file name to a slot in the renderer's sampler array. It is
        /// the caller's texture loader, so particle textures are cached, evicted and shared on the
        /// same terms as everything else.
        ///  resolveTexture returns the sampler slot to draw with and, through its out parameter,
        /// the storage index that slot came from -- the caller needs the second to keep the texture
        /// alive across eviction.
        explicit ParticleReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture);

        /// Walks \a sceneRoot and refills the quad list. Cheap enough to do every frame: the walk is
        /// over the loaded cell graph and the particle count is in the hundreds.
        void collect(osg::Node* sceneRoot);

        const std::vector<Vk::ParticleQuad>& quads() const { return mQuads; }

        /// Storage indices of every texture referenced this frame, for the caller's live set. Without
        /// this the eviction pass sees particle textures referenced by no mesh, no actor and no
        /// terrain chunk, decides they are dead, and replaces them with the white fallback -- so every
        /// flame in the game draws as a solid white quad.
        const std::vector<std::size_t>& textureIndices() const { return mTextureIndices; }

    private:
        std::function<uint32_t(const std::string&, std::size_t&)> mResolveTexture;
        std::vector<Vk::ParticleQuad> mQuads;
        std::vector<std::size_t> mTextureIndices;
    };
}

#endif
#endif
