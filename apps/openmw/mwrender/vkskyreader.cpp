#ifdef OPENMW_USE_VULKAN

#include "vkskyreader.hpp"

#include <string>

#include <osg/BoundingBox>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Material>
#include <osg/Matrix>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/TexMat>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec2f>
#include <osg/Vec4f>

#include <components/vk/vkmath.hpp>

#include "skyutil.hpp"
#include "vismask.hpp"
#include "vkskymesh.hpp"

namespace
{
    // Values of the `pass` uniform every sky stateset carries -- skyutil.cpp's Pass enum, line 43.
    //
    // Repeated here rather than included because that enum sits in an anonymous namespace inside the
    // .cpp, so nothing cross-checks these two numbers. They are worth the risk: the moons carry no
    // node mask of their own -- CelestialBody's visibleMask defaults to ~0u and Moon does not pass one
    // -- so the uniform is the only thing in the graph that tells a moon quad apart from a cloud layer
    // or the night sky dome.
    constexpr int sPassAtmosphereNight = 1;
    constexpr int sPassClouds = 2;
    constexpr int sPassMoon = 3;
    constexpr int sPassSun = 4;

    /// The sky state in force at the node currently being visited.
    ///
    /// Accumulated on the way down instead of searched for on the way up, which the sun and the moons
    /// do not need and the two meshes cannot do without. Their `pass` and `opacity` are not on their
    /// geometry at all: CloudUpdater and AtmosphereNightUpdater are attached to the *root* of each NIF
    /// instance (sky.cpp lines 337 and 320) and SceneUtil::StateSetUpdater::applyUpdate puts the
    /// stateset on the node the callback sits on, so everything that identifies a cloud shape is
    /// several levels above it.
    struct SkyState
    {
        int pass = -1;
        float opacity = 1.f;
        const osg::Material* material = nullptr;
        bool materialFixed = false;
        const osg::Texture2D* texture = nullptr;
        bool textureFixed = false;
        osg::Vec2f uvOffset;
    };

    /// Finds the sun disc, the two moons, the cloud layer and the night sky under a node and reads
    /// their live state out.
    ///
    /// apply(osg::Node&) rather than apply(osg::Geode&), for the reason vkparticlereader.cpp gives:
    /// osg::Drawable has been a Node since OSG 3.4 and these quads are added to the graph as child
    /// nodes directly, so a Geode override finds nothing at all.
    class Collector : public osg::NodeVisitor
    {
    public:
        Collector(std::vector<Vk::SkyElement>& out, std::vector<Vk::SkyMeshDraw>& meshes,
            std::vector<std::size_t>& textureIndices, MWRender::SkyMeshCache* meshCache,
            const std::function<uint32_t(const std::string&, std::size_t&)>& resolveTexture)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mOut(out)
            , mMeshes(meshes)
            , mTextureIndices(textureIndices)
            , mMeshCache(meshCache)
            , mResolveTexture(resolveTexture)
        {
            // Deliberately NOT setNodeMaskOverride, and here it matters more than it does for the
            // particles. Clearing a node mask is how the whole sky hides things: CelestialBody's
            // setVisible zeroes the transform's mask, and SkyManager clears Mask_Sky on the node above
            // everything. Overriding it draws the sun and both moons in every interior in the game,
            // through the ceiling, at whatever hour the last exterior cell left them at.
            //
            // The two meshes depend on it for more than that. mNextCloudMesh's mask is zero whenever
            // the weather crossfade is not running (sky.cpp line 796) and mAtmosphereNightNode's is
            // zero whenever the weather says it is not night (line 841), so honouring the mask is
            // what stops the previous weather's clouds being drawn on top of the current ones at full
            // strength, and what switches the stars off at dawn. Neither is a special case here
            // because neither needed to be written.
        }

        void apply(osg::Node& node) override
        {
            const bool pushed = pushSkyState(node);

            if (auto* geometry = dynamic_cast<osg::Geometry*>(&node))
                readGeometry(*geometry);

            traverse(node);

            if (pushed)
                mSkyState.pop_back();
        }

    private:
        /// Merges whatever sky state \a node introduces onto the stack. False if it introduces none.
        bool pushSkyState(const osg::Node& node)
        {
            const osg::StateSet* stateSet = node.getStateSet();
            if (stateSet == nullptr)
                return false;

            // The gate, and it is a performance gate rather than a correctness one. This visitor is
            // handed the whole scene root, so it walks every object, actor and terrain chunk in the
            // loaded cells; doing the state merge below on all of them would be paid every frame to
            // discover, every frame, that none of them is the sky.
            //
            // `pass` is the marker because the entire sky hangs under one stateset that has it:
            // mEarlyRenderBinRoot's, which sets it to -1 (sky.cpp line 359) so that a shape carrying
            // no pass of its own falls through the shader's branches rather than picking one. Nothing
            // outside the sky declares a uniform by that name.
            static const std::string sPassName = "pass";
            if (mSkyState.empty() && stateSet->getUniform(sPassName) == nullptr)
                return false;

            SkyState state = mSkyState.empty() ? SkyState() : mSkyState.back();
            mergeStateSet(state, *stateSet);
            mSkyState.push_back(state);
            return true;
        }

        static void mergeStateSet(SkyState& state, const osg::StateSet& stateSet)
        {
            readUniform(stateSet, "pass", state.pass);
            readUniform(stateSet, "opacity", state.opacity);

            // OVERRIDE is honoured rather than simply letting the nearest stateset win, and for the
            // clouds that is the difference between the weather's cloud texture and the one baked into
            // sky_clouds_01.nif. CloudUpdater rebinds the weather texture on unit 0 of the mesh's root
            // node every frame with ON | OVERRIDE (skyutil.cpp line 499), and the NIF's own shapes
            // below it each carry the NIF's texture on that same unit. Nearest-wins picks the NIF's,
            // and the sky then keeps one fixed cloud texture through every weather change in the game
            // -- which reads as the weather system being broken rather than as a state lookup being
            // wrong. The material carrying the fog tint is bound the same way (line 486).
            if (!state.textureFixed)
            {
                if (const auto* pair = stateSet.getTextureAttributePair(0, osg::StateAttribute::TEXTURE))
                {
                    if (const auto* texture = dynamic_cast<const osg::Texture2D*>(pair->first.get()))
                    {
                        state.texture = texture;
                        state.textureFixed = (pair->second & osg::StateAttribute::OVERRIDE) != 0;
                    }
                }
            }

            if (!state.materialFixed)
            {
                if (const auto* pair = stateSet.getAttributePair(osg::StateAttribute::MATERIAL))
                {
                    if (const auto* material = dynamic_cast<const osg::Material*>(pair->first.get()))
                    {
                        state.material = material;
                        state.materialFixed = (pair->second & osg::StateAttribute::OVERRIDE) != 0;
                    }
                }
            }

            // The cloud scroll, read off the texture matrix rather than recomputed. SkyManager
            // advances one float per frame and stores it as a translate on Y (skyutil.cpp lines
            // 478-481, applied at 504-505); recomputing it here would need the frame duration, the
            // weather's cloud speed and the Weather_Timescale_Clouds branch that ties the rate to the
            // game clock, all of which have already been folded into this one number.
            //
            // OSG matrices hold the translation in row 3, and GL reads that same memory as column 3 of
            // a column-major matrix, so this is the (x, y) the OSG vertex shader adds to the UV.
            if (const auto* texMat = dynamic_cast<const osg::TexMat*>(
                    stateSet.getTextureAttribute(0, osg::StateAttribute::TEXMAT)))
            {
                const osg::Matrix& matrix = texMat->getMatrix();
                state.uvOffset.set(static_cast<float>(matrix(3, 0)), static_cast<float>(matrix(3, 1)));
            }
        }

        /// The accumulated transform down this node path.
        ///
        /// The same hand-rolled walk vkparticlereader.cpp uses, and for the same two reasons, both of
        /// which apply here with more force because the sky is where they came from.
        ///
        /// The first is a crash. osg::computeLocalToWorld passes a **null** NodeVisitor to every
        /// Transform it walks, and MWRender::CameraRelativeTransform::computeLocalToWorldMatrix
        /// dereferences it without a check -- skyutil.cpp line 585, `nv->getVisitorType()`. The entire
        /// sky hangs under one of those, so calling the helper here would take the game down every
        /// frame rather than only in bad weather.
        ///
        /// The second is placement. That same function zeroes the translation for a relative reference
        /// frame, because the sky is drawn around the camera rather than in the world. Left at that,
        /// the sun, the moons, the clouds and the stars all sit around the world origin, so from
        /// anywhere else in Vvardenfell the player is outside his own sky looking back at it. Adding
        /// the transform's own view point back puts it around him.
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

        void readGeometry(osg::Geometry& geometry)
        {
            // The billboards are keyed off the geometry's OWN stateset, not off the inherited state
            // above. For the moons this is the stateset MoonUpdater installs --
            // SceneUtil::StateSetUpdater::applyUpdate calls node->setStateSet on the node the callback
            // is attached to, and Moon's constructor attaches it to the quad rather than the transform
            // (skyutil.cpp line 892). For the sun it is the one Sun's constructor writes directly
            // (line 674). Either way the discriminator is here and nowhere above, and keeping the test
            // that narrow is what stops a stray shape somewhere under a moon transform being drawn as
            // a second moon.
            const osg::StateSet* stateSet = geometry.getStateSet();
            if (stateSet != nullptr)
            {
                int pass = -1;
                const osg::Uniform* passUniform = stateSet->getUniform("pass");
                if (passUniform != nullptr && passUniform->get(pass))
                {
                    if (pass == sPassSun)
                    {
                        if (isSunDisc())
                            readSun(geometry, *stateSet);
                        return;
                    }

                    if (pass == sPassMoon)
                    {
                        readMoon(geometry, *stateSet);
                        return;
                    }
                }
            }

            // The meshes, off the inherited state. See SkyState for why they cannot use the branch
            // above.
            if (mSkyState.empty())
                return;

            const SkyState& state = mSkyState.back();
            if (state.pass == sPassClouds || state.pass == sPassAtmosphereNight)
                readMesh(geometry, state);
        }

        /// Whether the geometry just reached is the sun disc rather than the sun flash.
        ///
        /// There are two quads in the graph with pass == Sun on them: the disc (skyutil.cpp line 674)
        /// and the flash, the bright halo whose size tracks how much of the sun is occluded (line
        /// 819). Keying on the uniform alone matches both and draws the flash as a second sun 2.6
        /// times the size of the first, sitting exactly on top of it -- which reads as the sun being
        /// far too big rather than as two of them, so it is the kind of thing that gets blamed on the
        /// projection.
        ///
        /// What separates them is one step of the node path. CelestialBody's constructor adds the disc
        /// straight to the transform carrying Mask_Sun (line 647), whereas createSunFlash puts a plain
        /// osg::Group in between (line 804-810). Nothing else in the sky carries Mask_Sun, so the
        /// parent's mask is an exact test.
        bool isSunDisc() const
        {
            const osg::NodePath& path = getNodePath();
            if (path.size() < 2)
                return false;

            return path[path.size() - 2]->getNodeMask() == MWRender::Mask_Sun;
        }

        void readSun(osg::Geometry& geometry, const osg::StateSet& stateSet)
        {
            Vk::SkyElement element = {};
            if (!fillQuad(geometry, element))
                return;

            const uint32_t slot = resolveUnit(stateSet, 0);
            element.params[0] = slot;
            // The sun never samples the mask, but every index the shader could form has to be inside
            // the array whether or not the branch taking it runs.
            element.params[1] = slot;
            element.params[2] = 0;

            // paintSun in files/shaders/compatibility/sky.frag is two lines: the colour is the texture
            // and the alpha is the texture's times gl_FrontMaterial.diffuse.a. So the tint is white.
            //
            // The material's *emission* is deliberately left out even though SunUpdater writes the
            // weather's sun colour into it (skyutil.cpp line 125). paintSun has never read it for this
            // pass -- only paintAtmosphere and paintSunglare do -- and tinting the disc by it here
            // would give the Vulkan sun a different colour from the OSG one at every sunrise and in
            // every storm, which is exactly the kind of divergence reading the graph is meant to
            // prevent.
            element.colour[0] = 1.0f;
            element.colour[1] = 1.0f;
            element.colour[2] = 1.0f;
            element.colour[3] = readMaterialAlpha();

            mOut.push_back(element);
        }

        void readMoon(osg::Geometry& geometry, const osg::StateSet& stateSet)
        {
            Vk::SkyElement element = {};
            if (!fillQuad(geometry, element))
                return;

            // Unit 0 is the phase image and unit 1 the full-circle mask -- MoonUpdater::setDefaults,
            // skyutil.cpp lines 337-338. Reading them by file name is what makes the phase free:
            // Moon::setPhase has already turned the calendar into one of eight names (line 948-987),
            // so there is no second implementation of the lunar cycle here to drift out of step.
            element.params[0] = resolveUnit(stateSet, 0);
            element.params[1] = resolveUnit(stateSet, 1);
            element.params[2] = 1;

            // Everything the composite needs beyond the two images, already folded by MoonUpdater:
            // moonBlend is the moon's colour times its shadow blend, and atmosphereFade is the sky
            // colour with the moon's transparency in w. Passed through rather than reassembled,
            // because the shader multiplies them in a specific order that only makes sense as a pair.
            readUniform(stateSet, "moonBlend", element.moonBlend);
            readUniform(stateSet, "atmosphereFade", element.atmosphereFade);

            element.colour[0] = 1.0f;
            element.colour[1] = 1.0f;
            element.colour[2] = 1.0f;
            element.colour[3] = 1.0f;

            mOut.push_back(element);
        }

        /// One shape of the cloud layer or of the night sky.
        void readMesh(osg::Geometry& geometry, const SkyState& state)
        {
            if (mMeshCache == nullptr)
                return;

            const MWRender::SkyMeshBuffers buffers = mMeshCache->getOrUpload(geometry);
            if (buffers.indexCount == 0)
                return;

            Vk::SkyMeshDraw draw = {};
            draw.vertexBuffer = buffers.vertexBuffer;
            draw.indexBuffer = buffers.indexBuffer;
            draw.indexCount = buffers.indexCount;

            // Where this goes relative to the sun and the moons, decided here because this is the only
            // place that can see it. SkyManager adds the cloud group to the sky *after* the sun and
            // both moons (sky.cpp lines 322-330) and the night sky *before* them (line 307), and an
            // OSG render bin draws in graph order, so the clouds pass in front of a moon and the stars
            // sit behind it. The renderer draws the billboards and the meshes as two separate lists,
            // so without this the relative order of the two is simply lost -- and the way that shows
            // up is a full moon glowing through an overcast sky.
            draw.overBodies = state.pass == sPassClouds;

            // Column-major, and this is a verbatim copy of the matrix's memory rather than a
            // transpose. OSG uses the row-vector convention and stores element (row, col) at
            // linear index row * 4 + col; GLSL uses column vectors and reads linear index col * 4 + row
            // as (row, col). The two conventions are transposes of each other and so are the two
            // layouts, so they cancel exactly. Same conversion as osgMatrixToMat4 in
            // vkrenderingmanager.cpp; getting it wrong puts the sky in a plausible but wrong
            // orientation, which is much easier to mistake for a content problem than for a bug.
            const osg::Matrix localToWorld = computeLocalToWorld();
            for (int row = 0; row < 4; ++row)
            {
                for (int col = 0; col < 4; ++col)
                    draw.push.model.data[col * 4 + row] = static_cast<float>(localToWorld(col, row));
            }

            // The tint, decoded on the way through. SkyManager writes the weather's fog colour plus
            // 0.13 into this material every time the fog colour changes (sky.cpp lines 799-808), and
            // that number was authored in gamma space like every other colour in the game data. The
            // cloud texture beside it is decoded by the sampler because the loader gives it an _SRGB
            // format, so leaving this in gamma would multiply a linear texel by a gamma factor and get
            // neither -- the mistake vkmath.hpp's srgbToLinear exists to head off.
            //
            // The night sky has no tint: paintAtmosphereNight does not read the material at all, and
            // the emission is unused for that pass.
            osg::Vec4f emission(1.f, 1.f, 1.f, 1.f);
            if (state.material != nullptr)
                emission = state.material->getEmission(osg::Material::FRONT);

            draw.push.emission[0] = Vk::srgbToLinear(emission.r());
            draw.push.emission[1] = Vk::srgbToLinear(emission.g());
            draw.push.emission[2] = Vk::srgbToLinear(emission.b());
            // Not a colour, so it does not go through the transfer function -- it is the `opacity`
            // uniform, which is the weather crossfade for the clouds and mNightFade * mGlareView for
            // the stars.
            draw.push.emission[3] = state.opacity;

            draw.push.uvOffset[0] = state.uvOffset.x();
            draw.push.uvOffset[1] = state.uvOffset.y();

            draw.push.params[0] = resolveTextureSlot(state.texture);
            draw.push.params[1] = static_cast<uint32_t>(state.pass);

            mMeshes.push_back(draw);
        }

        /// The quad's world-space centre and its two half-extent axes.
        ///
        /// The extents come from the geometry's own bounding box rather than from the 450 in
        /// CelestialBody's constructor (skyutil.cpp line 646), because that 450 is only half the
        /// story: the moons multiply it by Moons_Masser_Size / 125 read out of the Morrowind ini
        /// (sky.cpp lines 324-327) and the sun does not. Taking the box means neither number is
        /// written down here and a mod that changes either is followed without anyone noticing.
        ///
        /// The axes come from the transform rather than from the camera, which is the one place this
        /// deliberately does not copy particle.vert. Moon::setState gives the quad a roll about the
        /// view direction (skyutil.cpp line 918-919) and that roll is what points a crescent's horns.
        /// A camera-facing billboard would face correctly and spin the phase image as the player
        /// turned, which looks like the moon rotating in place.
        bool fillQuad(osg::Geometry& geometry, Vk::SkyElement& element)
        {
            const osg::BoundingBox& box = geometry.getBoundingBox();
            if (!box.valid())
                return false;

            const osg::Matrix localToWorld = computeLocalToWorld();
            // Vec3d because osg::Matrix is Matrixd and getTrans answers in its own precision; osg::Vec3f
            // has no constructor from it.
            const osg::Vec3d centre = localToWorld.getTrans();

            // createTexturedQuad builds the quad in its local XY plane centred on the origin, and
            // pairs the vertex at (-0.5, -0.5) with uv (0, 0) -- skyutil.cpp lines 58-71. So u runs
            // along local +X and v along local +Y, and xMax/yMax are exactly the half extents the
            // shader expands a corner to.
            const osg::Vec3f right
                = osg::Matrix::transform3x3(osg::Vec3f(box.xMax(), 0.0f, 0.0f), localToWorld);
            const osg::Vec3f up
                = osg::Matrix::transform3x3(osg::Vec3f(0.0f, box.yMax(), 0.0f), localToWorld);

            element.position[0] = static_cast<float>(centre.x());
            element.position[1] = static_cast<float>(centre.y());
            element.position[2] = static_cast<float>(centre.z());
            element.position[3] = 1.0f;
            element.right[0] = right.x();
            element.right[1] = right.y();
            element.right[2] = right.z();
            element.up[0] = up.x();
            element.up[1] = up.y();
            element.up[2] = up.z();

            return true;
        }

        /// The sun's fade, which is gl_FrontMaterial.diffuse.a in the shader this is a port of.
        ///
        /// Searched up the node path rather than read off the quad, and that is not defensive coding.
        /// SunUpdater is attached to the transform, not to the geometry -- skyutil.cpp line 661 -- so
        /// the material is one level up and the quad's own stateset has none. Reading only the quad
        /// finds nothing, falls back to 1, and leaves the sun at full brightness through every storm,
        /// every sunset and every moment it should have faded out.
        float readMaterialAlpha() const
        {
            const osg::NodePath& path = getNodePath();

            // Nearest first: the innermost stateset wins, the way state inheritance does.
            for (auto it = path.rbegin(); it != path.rend(); ++it)
            {
                const osg::StateSet* stateSet = (*it)->getStateSet();
                if (stateSet == nullptr)
                    continue;

                const auto* material = dynamic_cast<const osg::Material*>(
                    stateSet->getAttribute(osg::StateAttribute::MATERIAL));
                if (material == nullptr)
                    continue;

                return material->getDiffuse(osg::Material::FRONT).a();
            }

            return 1.0f;
        }

        static void readUniform(const osg::StateSet& stateSet, const char* name, float (&out)[4])
        {
            // Left at zero when the uniform is absent, which is what the first frame looks like: the
            // update traversal has not run yet, so MoonUpdater has not installed its stateset. A moon
            // with a zero atmosphereFade is fully transparent, so that frame simply has no moons in
            // it -- which is the right answer and needs no special case.
            const osg::Uniform* uniform = stateSet.getUniform(name);
            if (uniform == nullptr)
                return;

            osg::Vec4f value;
            if (!uniform->get(value))
                return;

            out[0] = value.x();
            out[1] = value.y();
            out[2] = value.z();
            out[3] = value.w();
        }

        /// Scalar overloads of the same idea. Both leave \a out alone when the uniform is absent or
        /// holds another type, which is what keeps an inherited value from being clobbered by a
        /// stateset that says nothing about it.
        static void readUniform(const osg::StateSet& stateSet, const char* name, int& out)
        {
            const osg::Uniform* uniform = stateSet.getUniform(name);
            if (uniform == nullptr)
                return;

            int value = 0;
            if (uniform->get(value))
                out = value;
        }

        static void readUniform(const osg::StateSet& stateSet, const char* name, float& out)
        {
            const osg::Uniform* uniform = stateSet.getUniform(name);
            if (uniform == nullptr)
                return;

            float value = 0.f;
            if (uniform->get(value))
                out = value;
        }

        /// The texture on \a unit of \a stateSet, by file name, resolved through the caller's loader.
        uint32_t resolveUnit(const osg::StateSet& stateSet, unsigned int unit)
        {
            return resolveTextureSlot(dynamic_cast<const osg::Texture2D*>(
                stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE)));
        }

        /// \a texture by file name, resolved through the caller's loader.
        ///
        /// By name rather than by sharing OSG's texture object, because there is nothing to share: the
        /// OSG one is a GL texture in the other backend's context. ImageManager stamps the file name
        /// onto every image it loads, which is what makes this possible at all -- and for the moons it
        /// is also what carries the phase, and for the clouds the weather.
        uint32_t resolveTextureSlot(const osg::Texture2D* texture)
        {
            if (texture == nullptr || texture->getImage() == nullptr)
                return 0;

            const std::string& name = texture->getImage()->getFileName();
            if (name.empty())
                return 0;

            std::size_t storageIndex = static_cast<std::size_t>(-1);
            const uint32_t slot = mResolveTexture(name, storageIndex);
            if (storageIndex != static_cast<std::size_t>(-1))
                mTextureIndices.push_back(storageIndex);

            return slot;
        }

        std::vector<Vk::SkyElement>& mOut;
        std::vector<Vk::SkyMeshDraw>& mMeshes;
        std::vector<std::size_t>& mTextureIndices;
        MWRender::SkyMeshCache* mMeshCache;
        const std::function<uint32_t(const std::string&, std::size_t&)>& mResolveTexture;
        // Empty everywhere except under the sky. Never more than a handful deep, so it is a vector
        // rather than anything cleverer.
        std::vector<SkyState> mSkyState;
    };
}

namespace MWRender
{
    SkyReader::SkyReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture,
        Vk::Device& device, Vk::CommandPool& commandPool)
        : mResolveTexture(std::move(resolveTexture))
        , mMeshCache(std::make_unique<SkyMeshCache>(device, commandPool))
    {
    }

    SkyReader::~SkyReader() = default;

    void SkyReader::collect(osg::Node* sceneRoot)
    {
        // Cleared and rebuilt rather than tracked, for the same reason the particles are: the
        // simulation that owns these is re-read every frame, so there is no state here that can go
        // stale. It also means an interior, where the sky is hidden, empties the lists by itself.
        //
        // The mesh *buffers* are not cleared with them. Those live in mMeshCache and are uploaded once;
        // what is rebuilt here is only which of them to draw and with what transform, texture, opacity
        // and scroll. Re-uploading a sky mesh every frame would be several hundred kilobytes of
        // staging traffic for geometry that never changes.
        mElements.clear();
        mMeshes.clear();
        mTextureIndices.clear();
        if (sceneRoot == nullptr)
            return;

        Collector collector(mElements, mMeshes, mTextureIndices, mMeshCache.get(), mResolveTexture);
        sceneRoot->accept(collector);
    }
}

#endif
