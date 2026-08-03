#ifndef OPENMW_MWRENDER_VKPARTICLEREADER_H
#define OPENMW_MWRENDER_VKPARTICLEREADER_H

#ifdef OPENMW_USE_VULKAN

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
        explicit ParticleReader(std::function<uint32_t(const std::string&)> resolveTexture);

        /// Walks \a sceneRoot and refills the quad list. Cheap enough to do every frame: the walk is
        /// over the loaded cell graph and the particle count is in the hundreds.
        void collect(osg::Node* sceneRoot);

        const std::vector<Vk::ParticleQuad>& quads() const { return mQuads; }

    private:
        std::function<uint32_t(const std::string&)> mResolveTexture;
        std::vector<Vk::ParticleQuad> mQuads;
    };
}

#endif
#endif
