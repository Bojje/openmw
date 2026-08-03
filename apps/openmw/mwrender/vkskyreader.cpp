#ifdef OPENMW_USE_VULKAN

#include "vkskyreader.hpp"

#include <osg/BoundingBox>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Material>
#include <osg/Matrix>
#include <osg/NodeVisitor>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec4f>

#include "skyutil.hpp"
#include "vismask.hpp"

namespace
{
    // Values of the `pass` uniform every sky stateset carries -- skyutil.cpp's Pass enum, line 43.
    //
    // Repeated here rather than included because that enum sits in an anonymous namespace inside the
    // .cpp, so nothing cross-checks these two numbers. They are worth the risk: the moons carry no
    // node mask of their own -- CelestialBody's visibleMask defaults to ~0u and Moon does not pass one
    // -- so the uniform is the only thing in the graph that tells a moon quad apart from a cloud layer
    // or the night sky dome.
    constexpr int sPassMoon = 3;
    constexpr int sPassSun = 4;

    /// Finds the sun disc and the two moons under a node and reads their live state out.
    ///
    /// apply(osg::Node&) rather than apply(osg::Geode&), for the reason vkparticlereader.cpp gives:
    /// osg::Drawable has been a Node since OSG 3.4 and these quads are added to the graph as child
    /// nodes directly, so a Geode override finds nothing at all.
    class Collector : public osg::NodeVisitor
    {
    public:
        Collector(std::vector<Vk::SkyElement>& out, std::vector<std::size_t>& textureIndices,
            const std::function<uint32_t(const std::string&, std::size_t&)>& resolveTexture)
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            , mOut(out)
            , mTextureIndices(textureIndices)
            , mResolveTexture(resolveTexture)
        {
            // Deliberately NOT setNodeMaskOverride, and here it matters more than it does for the
            // particles. Clearing a node mask is how the whole sky hides things: CelestialBody's
            // setVisible zeroes the transform's mask, and SkyManager clears Mask_Sky on the node above
            // everything. Overriding it draws the sun and both moons in every interior in the game,
            // through the ceiling, at whatever hour the last exterior cell left them at.
        }

        void apply(osg::Node& node) override
        {
            if (auto* geometry = dynamic_cast<osg::Geometry*>(&node))
                readGeometry(*geometry);

            traverse(node);
        }

    private:
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
        /// the sun and both moons orbit the world origin, so from anywhere else in Vvardenfell they
        /// sit in a fixed patch of sky that has nothing to do with the time of day. Adding the
        /// transform's own view point back puts them around the player.
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
            // The geometry's own stateset, not an inherited one. For the moons this is the stateset
            // MoonUpdater installs -- SceneUtil::StateSetUpdater::applyUpdate calls
            // node->setStateSet on the node the callback is attached to, and Moon's constructor
            // attaches it to the quad rather than the transform (skyutil.cpp line 892). For the sun
            // it is the one Sun's constructor writes directly (line 674). Either way the discriminator
            // is here and nowhere above.
            const osg::StateSet* stateSet = geometry.getStateSet();
            if (stateSet == nullptr)
                return;

            const osg::Uniform* passUniform = stateSet->getUniform("pass");
            if (passUniform == nullptr)
                return;

            int pass = -1;
            if (!passUniform->get(pass))
                return;

            if (pass == sPassSun)
            {
                if (isSunDisc())
                    readSun(geometry, *stateSet);
            }
            else if (pass == sPassMoon)
            {
                readMoon(geometry, *stateSet);
            }
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

        /// The texture on \a unit, by file name, resolved through the caller's loader.
        ///
        /// By name rather than by sharing OSG's texture object, because there is nothing to share: the
        /// OSG one is a GL texture in the other backend's context. ImageManager stamps the file name
        /// onto every image it loads, which is what makes this possible at all -- and for the moons it
        /// is also what carries the phase.
        uint32_t resolveUnit(const osg::StateSet& stateSet, unsigned int unit)
        {
            const auto* texture = dynamic_cast<const osg::Texture2D*>(
                stateSet.getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
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
        std::vector<std::size_t>& mTextureIndices;
        const std::function<uint32_t(const std::string&, std::size_t&)>& mResolveTexture;
    };
}

namespace MWRender
{
    SkyReader::SkyReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture)
        : mResolveTexture(std::move(resolveTexture))
    {
    }

    void SkyReader::collect(osg::Node* sceneRoot)
    {
        // Cleared and rebuilt rather than tracked, for the same reason the particles are: the
        // simulation that owns these is re-read every frame, so there is no state here that can go
        // stale. It also means an interior, where the sky is hidden, empties the list by itself.
        mElements.clear();
        mTextureIndices.clear();
        if (sceneRoot == nullptr)
            return;

        Collector collector(mElements, mTextureIndices, mResolveTexture);
        sceneRoot->accept(collector);
    }
}

#endif
