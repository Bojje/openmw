#ifdef OPENMW_USE_VULKAN

#include "vkparticlereader.hpp"

#include <osg/Image>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>

namespace
{
    /// Finds every osgParticle::ParticleSystem under a node and reads its live particles out.
    ///
    /// apply(osg::Node&), not apply(osg::Geode&). NifOsg adds the system to the graph as a child node
    /// directly -- osg::Drawable has been a Node since OSG 3.4 -- so a Geode override finds nothing
    /// and reports a world with no fire in it. That exact mistake cost a probe and nearly a wrong
    /// conclusion about whether OSG was simulating at all.
    class Collector : public osg::NodeVisitor
    {
    public:
        Collector(std::vector<Vk::ParticleQuad>& out, std::vector<std::size_t>& textureIndices,
            const std::function<uint32_t(const std::string&, std::size_t&)>& resolveTexture)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mOut(out)
            , mTextureIndices(textureIndices)
            , mResolveTexture(resolveTexture)
        {
            // Deliberately NOT setNodeMaskOverride. The first version had it, on the reasoning that a
            // node mask says what a node is hidden from rather than whether it exists -- which is
            // true in general and wrong here. OpenMW switches the weather particles off by clearing
            // their node mask, so overriding it resurrects them: rain fell inside a tavern, indoors,
            // through the ceiling. Honouring the mask is what makes this read the world OSG is
            // actually showing rather than every system that happens to be in the graph.
        }

        void apply(osg::Node& node) override
        {
            if (auto* system = dynamic_cast<osgParticle::ParticleSystem*>(&node))
                readSystem(*system);

            traverse(node);
        }

    private:
        void readSystem(osgParticle::ParticleSystem& system)
        {
            const uint32_t texture = resolveSystemTexture(system);

            // osgParticle keeps particles in local coordinates when the system's reference frame is
            // relative, which is what Morrowind's flame nodes use -- their ParticleFlag_LocalSpace is
            // set. The node path from the scene root supplies the rest.
            const osg::Matrix localToWorld = osg::computeLocalToWorld(getNodePath());

            const int count = static_cast<int>(system.numParticles());
            for (int i = 0; i < count; ++i)
            {
                const osgParticle::Particle* particle = system.getParticle(i);
                if (particle == nullptr || !particle->isAlive())
                    continue;

                // getCurrentSize and getCurrentColor rather than the template's: both are animated
                // over a particle's life by the modifiers -- NiParticleGrowFade and
                // NiParticleColorModifier -- and reading the template instead gives every particle the
                // same size and colour for its whole life, which is most of what makes a flame look
                // like a flame.
                const osg::Vec3 world = particle->getPosition() * localToWorld;
                const osg::Vec4 colour = particle->getCurrentColor();
                const float size = particle->getCurrentSize();
                if (size <= 0.0f || colour.a() <= 0.0f)
                    continue;

                Vk::ParticleQuad quad = {};
                quad.position[0] = static_cast<float>(world.x());
                quad.position[1] = static_cast<float>(world.y());
                quad.position[2] = static_cast<float>(world.z());
                quad.size = size * 0.5f; // getCurrentSize is a diameter; the shader wants a half extent
                quad.colour[0] = colour.r();
                quad.colour[1] = colour.g();
                quad.colour[2] = colour.b();
                // osgParticle fades alpha over life through the same colour interpolation, so this
                // already carries the fade.
                quad.colour[3] = colour.a();
                quad.textureIndex = texture;

                mOut.push_back(quad);
            }
        }

        /// The system's texture, by file name, resolved through the caller's loader.
        ///
        /// By name rather than by sharing OSG's texture object, because there is nothing to share: the
        /// OSG one is a GL texture in the other backend's context. ImageManager stamps the file name
        /// onto every image it loads, which is what makes this possible at all.
        ///
        /// Searched up the node path rather than read off the system, and that is not defensive
        /// coding. NifOsg applies a particle system's drawable properties to its *parent* node --
        /// `applyDrawableProperties(parentNode, ...)` in handleParticleSystem -- so the system's own
        /// stateset has no texture on it and never will. Reading only the system gives every effect
        /// in the game the white fallback, which looks like a solid white square where the flame
        /// should be.
        uint32_t resolveSystemTexture(osgParticle::ParticleSystem& system)
        {
            const osg::NodePath& path = getNodePath();

            // Nearest first: the innermost stateset wins, the way state inheritance does.
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                const osg::StateSet* stateSet = (*it)->getStateSet();
                if (stateSet == nullptr)
                    continue;

                const auto* texture = dynamic_cast<const osg::Texture2D*>(
                    stateSet->getTextureAttribute(0, osg::StateAttribute::TEXTURE));
                if (texture == nullptr || texture->getImage() == nullptr)
                    continue;

                const std::string& name = texture->getImage()->getFileName();
                if (!name.empty())
                {
                    std::size_t storageIndex = static_cast<std::size_t>(-1);
                    const uint32_t slot = mResolveTexture(name, storageIndex);
                    if (storageIndex != static_cast<std::size_t>(-1))
                        mTextureIndices.push_back(storageIndex);
                    return slot;
                }
            }

            (void)system;
            return 0;
        }

        std::vector<Vk::ParticleQuad>& mOut;
        std::vector<std::size_t>& mTextureIndices;
        const std::function<uint32_t(const std::string&, std::size_t&)>& mResolveTexture;
    };
}

namespace MWRender
{
    ParticleReader::ParticleReader(
        std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture)
        : mResolveTexture(std::move(resolveTexture))
    {
    }

    void ParticleReader::collect(osg::Node* sceneRoot)
    {
        // Cleared and rebuilt rather than tracked, for the same reason actors are: the simulation that
        // owns these is re-read every frame, so there is no state here that can go stale.
        mQuads.clear();
        mTextureIndices.clear();
        if (sceneRoot == nullptr)
            return;

        Collector collector(mQuads, mTextureIndices, mResolveTexture);
        sceneRoot->accept(collector);
    }
}

#endif
