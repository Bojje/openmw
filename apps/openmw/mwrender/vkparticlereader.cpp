#ifdef OPENMW_USE_VULKAN

#include "vkparticlereader.hpp"

#include <algorithm>
#include <cmath>

#include <osg/BlendFunc>
#include <osg/Group>
#include <osg/Image>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Matrixd>
#include <osgParticle/Particle>
#include <osgParticle/ParticleSystem>

#include <components/sceneutil/lightcontroller.hpp>
#include <components/sceneutil/lightmanager.hpp>

#include "skyutil.hpp"

namespace
{
    /// The sRGB transfer function, matching writeColor in vklightcollector.cpp.
    ///
    /// Particle colours arrive authored in gamma space, the same as every other colour in the game
    /// data. The texture beside them is already decoded by the sampler, because the loader gives it an
    /// _SRGB format, so leaving the colour undecoded multiplies a linear texel by a gamma factor and
    /// gets neither.
    float srgbToLinear(float c)
    {
        const float a = std::abs(c);
        return a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
    }

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
            const std::function<std::size_t(const std::string&)>& resolveTexture)
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
        /// The accumulated transform down this node path.
        ///
        /// Hand-rolled rather than osg::computeLocalToWorld, for two reasons and the first is a crash.
        /// That helper passes a **null** NodeVisitor to every Transform it walks, and
        /// MWRender::CameraRelativeTransform::computeLocalToWorldMatrix dereferences it without a
        /// check -- skyutil.cpp, `nv->getVisitorType()`. The sky's rain, snow, ash and blight systems
        /// all hang under one of those, so walking the graph during any bad weather would take the
        /// game down. It was never seen because the test harness pins the weather clear.
        ///
        /// The second reason is placement. That same function zeroes the translation for a relative
        /// reference frame, because the sky is drawn around the camera rather than in the world. Left
        /// at that, every raindrop in the game is emitted around the world origin. Adding the
        /// transform's own view point back is what puts the weather where the player is.
        osg::Matrix computeLocalToWorld()
        {
            const osg::NodePath& path = getNodePath();

            osg::Matrix matrix;
            osg::Vec3f cameraRelativeOrigin;
            bool cameraRelative = false;

            for (const osg::Node* node : path)
            {
                if (const osg::Transform* transform = node->asTransform())
                {
                    // `this` rather than nullptr. It is a NODE_VISITOR rather than a CULL_VISITOR, so
                    // CameraRelativeTransform skips its view point update and takes the safe branch.
                    transform->computeLocalToWorldMatrix(matrix, this);
                }

                if (const auto* relative = dynamic_cast<const MWRender::CameraRelativeTransform*>(node))
                {
                    cameraRelative = true;
                    cameraRelativeOrigin = relative->getLastViewPoint();
                }
            }

            if (cameraRelative)
                matrix.postMultTranslate(cameraRelativeOrigin);

            return matrix;
        }

        void readSystem(osgParticle::ParticleSystem& system)
        {
            const std::size_t texture = resolveSystemTexture(system);
            const bool additive = resolveAdditive();
            const float brightness = resolveLightBrightness();

            // FIXED alignment means the effect author chose the quad's orientation and it is not a
            // billboard. Rain is the case that matters: (0.1, 0, 0) by (0, 0, -1) is a thin vertical
            // streak, and a camera-facing square in its place is a white blob.
            const bool fixedAlignment
                = system.getParticleAlignment() == osgParticle::ParticleSystem::FIXED;

            // osgParticle keeps particles in local coordinates when the system's reference frame is
            // relative, which is what Morrowind's flame nodes use -- their ParticleFlag_LocalSpace is
            // set. The node path from the scene root supplies the rest.
            const osg::Matrix localToWorld = computeLocalToWorld();

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
                // Opacity is two numbers in osgParticle, not one. getCurrentColor's alpha is the colour
                // curve's, and getCurrentAlpha is a separate range the operators drive; Particle::render
                // multiplies them together and so must this. Missing it left the weather at full
                // opacity always, because MWRender's WeatherAlphaOperator fades rain and snow in and out
                // through setAlphaRange and touches the colour not at all -- so a drizzle came down as
                // hard as a storm.
                const float alpha = colour.a() * particle->getCurrentAlpha();
                if (size <= 0.0f || alpha <= 0.0f)
                    continue;

                Vk::ParticleQuad quad = {};
                quad.position[0] = static_cast<float>(world.x());
                quad.position[1] = static_cast<float>(world.y());
                quad.position[2] = static_cast<float>(world.z());
                // getCurrentSize is already a half extent -- osgParticle builds its quad from
                // +/- _current_size along the alignment axes. Halving it again drew every flame at
                // half the size OSG does, which measured as roughly 40 screen pixels against 90 on
                // the same torch in the same frame.
                quad.size = size;
                quad.colour[0] = srgbToLinear(colour.r()) * brightness;
                quad.colour[1] = srgbToLinear(colour.g()) * brightness;
                quad.colour[2] = srgbToLinear(colour.b()) * brightness;
                quad.colour[3] = alpha;
                // The *storage* index, not the sampler slot. render() rewrites this field through
                // ParticleReader::resolveTextureSlots once the frame's texture sync has run.
                // Resolving it here means resolving it before that sync, and the sync renumbers the
                // whole sampler array instead of appending to it -- so on any frame a cell unloaded,
                // every flame, spark and raindrop in the world drew with somebody else's texture.
                //
                // Truncating to 32 bits loses nothing that matters: the only value that does not
                // survive it is the no-texture sentinel, and what it becomes -- 0xFFFFFFFF -- is
                // past the end of the slot table, which resolves to the white fallback exactly as
                // the sentinel asks for.
                quad.textureIndex = static_cast<uint32_t>(texture);
                quad.additive = additive ? 1u : 0u;

                if (fixedAlignment)
                {
                    // The align vectors are in the system's own frame, so they need the same transform
                    // the positions got -- the rotation part of it. osgParticle scales them by the
                    // particle's current size exactly as it scales a billboard's camera axes, so the
                    // two paths differ only in where the axes come from.
                    const osg::Vec3 x
                        = osg::Matrix::transform3x3(system.getAlignVectorX(), localToWorld) * size;
                    const osg::Vec3 y
                        = osg::Matrix::transform3x3(system.getAlignVectorY(), localToWorld) * size;
                    quad.axisX[0] = static_cast<float>(x.x());
                    quad.axisX[1] = static_cast<float>(x.y());
                    quad.axisX[2] = static_cast<float>(x.z());
                    quad.axisY[0] = static_cast<float>(y.x());
                    quad.axisY[1] = static_cast<float>(y.y());
                    quad.axisY[2] = static_cast<float>(y.z());
                    // The flag, rather than testing the axes for zero in the shader: a fixed system
                    // whose particles have shrunk to nothing would otherwise flip to billboarding for
                    // one frame on the way out.
                    quad.axisX[3] = 1.0f;
                }

                mOut.push_back(quad);
            }
        }

        /// How bright the light this effect belongs to is right now, or 1 if it has none.
        ///
        /// This is the one place the Vulkan renderer deliberately draws something OSG does not. In
        /// Morrowind a fire is two unrelated objects that happen to sit on the same node: an
        /// osgParticle system for the flame, and an ESM light whose brightness a LightController
        /// flickers between roughly 0.25 and 1 fifteen times a second. Nothing connects them, so the
        /// walls behind a torch pulse while the flame itself is perfectly steady -- which is what a
        /// video light looks like, not a fire, and it is the specific thing that was reported as the
        /// fire and the light "not matching".
        ///
        /// A fire's flame and the light it casts are the same emission, so they are driven from the
        /// same number here. The number is read, not invented: LightController has already computed it
        /// this frame for the light, and this only asks what it decided.
        ///
        /// Found by looking for a LightSource among the siblings on the way up the node path, because
        /// that is how the two are related -- Animation::addExtraLight adds the light as a child of the
        /// same group the mesh is under, so they are siblings rather than one being above the other.
        /// The first one found wins, so a torch in a lantern-lit room follows its own flame.
        float resolveLightBrightness()
        {
            const osg::NodePath& path = getNodePath();
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                osg::Group* group = (*it)->asGroup();
                if (group == nullptr)
                    continue;

                const unsigned int children = group->getNumChildren();
                for (unsigned int i = 0; i < children; ++i)
                {
                    auto* lightSource = dynamic_cast<SceneUtil::LightSource*>(group->getChild(i));
                    if (lightSource == nullptr)
                        continue;

                    const auto* controller
                        = dynamic_cast<const SceneUtil::LightController*>(lightSource->getUpdateCallback());
                    if (controller == nullptr)
                        continue;

                    // Actor fade is in here for the same reason the controller applies it to the light:
                    // a corpse dissolving away should take its torch flame with it.
                    return controller->getBrightness() * lightSource->getActorFade();
                }
            }

            // Smoke plumes, spell effects and the weather have no light of their own and must not be
            // modulated by whatever light happens to be nearby.
            return 1.0f;
        }

        /// Whether this system was authored to blend additively.
        ///
        /// Read rather than assumed. Morrowind authors most effects with SRC_ALPHA and
        /// ONE_MINUS_SRC_ALPHA -- including flames and smoke -- and only some with a destination of
        /// ONE. Drawing everything additively makes smoke *glow* on a dark wall instead of darkening
        /// it, which is exactly how it looked before this existed.
        ///
        /// NifOsg puts the BlendFunc on the same stateset as the texture, which is the particle
        /// system's parent node rather than the system -- see resolveSystemTexture.
        bool resolveAdditive()
        {
            const osg::NodePath& path = getNodePath();
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                const osg::StateSet* stateSet = (*it)->getStateSet();
                if (stateSet == nullptr)
                    continue;

                const auto* blend = dynamic_cast<const osg::BlendFunc*>(
                    stateSet->getAttribute(osg::StateAttribute::BLENDFUNC));
                if (blend == nullptr)
                    continue;

                return blend->getDestination() == GL_ONE;
            }

            return false;
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
        std::size_t resolveSystemTexture(osgParticle::ParticleSystem& system)
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
                    const std::size_t storageIndex = mResolveTexture(name);
                    if (storageIndex != static_cast<std::size_t>(-1))
                        mTextureIndices.push_back(storageIndex);
                    return storageIndex;
                }
            }

            (void)system;
            // All ones rather than 0. This used to be able to answer 0 safely only because it was
            // answering a *slot*, and slot 0 is the white fallback; as a storage index 0 is a real
            // texture -- whichever one the renderer loaded first -- so returning it here would give
            // every untextured system a texture at random. The caller's textureSlot turns this into
            // the same white fallback the old 0 meant.
            return static_cast<std::size_t>(-1);
        }

        std::vector<Vk::ParticleQuad>& mOut;
        std::vector<std::size_t>& mTextureIndices;
        const std::function<std::size_t(const std::string&)>& mResolveTexture;
    };
}

namespace MWRender
{
    ParticleReader::ParticleReader(std::function<std::size_t(const std::string&)> resolveTexture)
        : mResolveTexture(std::move(resolveTexture))
    {
    }

    void ParticleReader::sortForCamera(const osg::Vec3f& cameraPosition)
    {
        mRuns.clear();
        if (mQuads.empty())
            return;

        // Back to front, which is what a translucent surface needs and what OSG's transparent bin
        // does. Sorted by squared distance because the ordering is all that matters and a square root
        // per quad is not.
        //
        // One list rather than two batches by blend mode. Sorting the modes separately would be
        // cheaper and would put every additive spark in front of every alpha blended flame regardless
        // of where they actually are.
        std::sort(mQuads.begin(), mQuads.end(),
            [&cameraPosition](const Vk::ParticleQuad& a, const Vk::ParticleQuad& b) {
                const float ax = a.position[0] - cameraPosition.x();
                const float ay = a.position[1] - cameraPosition.y();
                const float az = a.position[2] - cameraPosition.z();
                const float bx = b.position[0] - cameraPosition.x();
                const float by = b.position[1] - cameraPosition.y();
                const float bz = b.position[2] - cameraPosition.z();
                return (ax * ax + ay * ay + az * az) > (bx * bx + by * by + bz * bz);
            });

        // Broken into runs wherever the mode changes, so the draw loop switches pipeline as rarely as
        // the sort order allows.
        Vk::ParticleRun run = { 0, 0, mQuads[0].additive != 0u };
        for (std::size_t i = 0; i < mQuads.size(); ++i)
        {
            const bool additive = mQuads[i].additive != 0u;
            if (additive != run.additive)
            {
                mRuns.push_back(run);
                run = { static_cast<uint32_t>(i), 0, additive };
            }
            ++run.count;
        }
        mRuns.push_back(run);
    }

    void ParticleReader::resolveTextureSlots(const std::function<uint32_t(std::size_t)>& slotOf)
    {
        // One pass over the frame's live quads and nothing else. The count is in the hundreds even
        // in a torch-lit interior in bad weather, so this is a few microseconds on a Steam Deck and
        // it does not grow with the size of the loaded cells -- unlike the collect() that filled the
        // list, which walks the whole scene graph and is the part worth watching.
        //
        // The widening from uint32_t to std::size_t is what turns the no-texture sentinel back into
        // something the caller answers the white fallback for; see where the field is written.
        for (Vk::ParticleQuad& quad : mQuads)
            quad.textureIndex = slotOf(quad.textureIndex);
    }

    void ParticleReader::collect(osg::Node* sceneRoot)
    {
        // Cleared and rebuilt rather than tracked, for the same reason actors are: the simulation that
        // owns these is re-read every frame, so there is no state here that can go stale.
        mQuads.clear();
        mRuns.clear();
        mTextureIndices.clear();
        if (sceneRoot == nullptr)
            return;

        Collector collector(mQuads, mTextureIndices, mResolveTexture);
        sceneRoot->accept(collector);
    }
}

#endif
