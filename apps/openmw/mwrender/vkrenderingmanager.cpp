#ifdef OPENMW_USE_VULKAN

#include "vkrenderingmanager.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <cmath>
#include <cstring>
#include <exception>

#include <osg/Matrixf>
#include <osg/Quat>
#include <osg/Vec3f>

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadland.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <osg/TexMat>

#include <components/misc/strings/algorithm.hpp>
#include <components/nif/niffile.hpp>
#include <components/nifvk/meshconverter.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vk/vkbuffer.hpp>
#include <algorithm>
#include <cmath>

// std::clamp and std::acos for the sun angle; osg::DegreesToRadians for the falloff, which
// is the unit Weather_Sun_Glare_Fader_Angle_Max is authored in; osg::Vec4f because
// Fallback::Map::getColour answers in one.
#include <osg/Math>
#include <osg/Vec4f>

#include <components/fallback/fallback.hpp>
#include <components/vk/vkmath.hpp>
#include <components/vk/vkrenderer.hpp>
#include <components/vk/vktexture.hpp>

#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>
#include <components/esm3/loadclot.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadrace.hpp>
#include <components/esm3/loadweap.hpp>

#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwmechanics/weapontype.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/inventorystore.hpp"

#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/skeleton.hpp>

#include "animation.hpp"
#include "npcanimation.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "camera.hpp"
#include "vklandcomposite.hpp"
#include "vkglowreader.hpp"
#include "vkparticlereader.hpp"
#include "vkskyreader.hpp"
#include "vkterrainbuilder.hpp"

namespace
{
    // GL pixel format values as reported by osg::Image::getPixelFormat(). Spelled out here rather than
    // pulled from a GL header so this file needs no GL include.
    constexpr unsigned int sGlRgb = 0x1907;
    constexpr unsigned int sGlRgba = 0x1908;
    constexpr unsigned int sGlBgra = 0x80E1;
    constexpr unsigned int sGlDxt1Rgb = 0x83F0;
    constexpr unsigned int sGlDxt1Rgba = 0x83F1;
    constexpr unsigned int sGlDxt3 = 0x83F2;
    constexpr unsigned int sGlDxt5 = 0x83F3;

    // Morrowind's textures are mostly DDS/S3TC. Vulkan consumes BC1/BC2/BC3 blocks directly, so
    // compressed data is uploaded untouched. OpenMW already requires S3TC support for these assets,
    // so assuming the textureCompressionBC feature matches the engine's existing requirements.
    // Returns VK_FORMAT_UNDEFINED for anything unhandled, which the caller treats as "untextured".
    // sRGB formats, not UNORM: Morrowind's diffuse textures are sRGB-encoded. Sampling them as UNORM
    // hands the shader sRGB values as though they were linear, so lighting runs on wrong values and the
    // swapchain's sRGB store encodes them a second time -- the result is visibly washed out. The _SRGB
    // formats make the GPU convert to linear on sample, which is what the lighting maths expects.
    // Normal/material G-buffer targets stay UNORM; only colour is sRGB.
    //
    // \a srgb is false for the one texture in the game that is not a colour: the water normal map. A
    // tangent-space normal is a direction packed into [0, 1], and the sRGB transfer function turns the
    // flat value 0.5 into 0.21 -- so every normal in the map tilts hard the same way and the sea reads
    // as lit from underneath. Nothing about the file says which kind it is, so the caller has to.
    VkFormat toVkFormat(unsigned int glPixelFormat, bool srgb = true)
    {
        switch (glPixelFormat)
        {
            case sGlDxt1Rgb:
                return srgb ? VK_FORMAT_BC1_RGB_SRGB_BLOCK : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
            case sGlDxt1Rgba:
                return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            case sGlDxt3: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
            case sGlDxt5: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            case sGlRgba: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            case sGlBgra: return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
            // Three-channel formats are poorly supported as sampled images; they would need expanding
            // to four channels before upload, which is not implemented yet.
            case sGlRgb: return VK_FORMAT_UNDEFINED;
            default: return VK_FORMAT_UNDEFINED;
        }
    }

    // Byte size of mip level 0.
    //
    // osg::Image::getTotalSizeInBytes() cannot be used for this: for block-compressed data it reports
    // the size as though the image were uncompressed (a 128x128 DXT1 image reports 32768 bytes when
    // level 0 actually occupies 8192), and it also spans the whole mip chain. Trusting it makes the
    // upload memcpy read far past the end of the decoded buffer.
    VkDeviceSize levelZeroSizeInBytes(VkFormat format, uint32_t width, uint32_t height)
    {
        const VkDeviceSize blocksWide = (width + 3) / 4;
        const VkDeviceSize blocksHigh = (height + 3) / 4;

        switch (format)
        {
            // BC1 packs a 4x4 block into 8 bytes; BC2/BC3 add an alpha block, so 16. The UNORM and
            // SRGB variants of a format differ only in how samples are interpreted, never in size.
            case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
            case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
            case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
                return blocksWide * blocksHigh * 8;
            case VK_FORMAT_BC2_UNORM_BLOCK:
            case VK_FORMAT_BC2_SRGB_BLOCK:
            case VK_FORMAT_BC3_UNORM_BLOCK:
            case VK_FORMAT_BC3_SRGB_BLOCK:
                return blocksWide * blocksHigh * 16;
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
                return static_cast<VkDeviceSize>(width) * height * 4;
            default:
                return 0;
        }
    }

    // World transform for a cell reference. The rotation convention mirrors the anonymous helpers in
    // apps/openmw/mwworld/scene.cpp (makeActorOsgQuat / makeInversedOrderObjectOsgQuat) so that the
    // Vulkan renderer places objects identically to the OSG renderer.
    void makeObjectTransform(const MWWorld::ConstPtr& ptr, float out[16])
    {
        const ESM::Position& position = ptr.getRefData().getPosition();
        const float scale = ptr.getCellRef().getScale();

        osg::Quat rotation;
        if (ptr.getClass().isActor())
        {
            rotation = osg::Quat(position.rot[2], osg::Vec3f(0, 0, -1));
        }
        else
        {
            rotation = osg::Quat(position.rot[0], osg::Vec3f(-1, 0, 0))
                * osg::Quat(position.rot[1], osg::Vec3f(0, -1, 0))
                * osg::Quat(position.rot[2], osg::Vec3f(0, 0, -1));
        }

        // OSG uses the row-vector convention (v * M), so scale is applied first and translation last.
        const osg::Matrixf matrix = osg::Matrixf::scale(scale, scale, scale)
            * osg::Matrixf::rotate(rotation)
            * osg::Matrixf::translate(position.pos[0], position.pos[1], position.pos[2]);

        // Transpose into the column-vector convention the shaders expect.
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                out[col * 4 + row] = matrix(col, row);
    }

    // The same transform, read out of the OSG node the object is actually drawn through instead of out
    // of the reference record.
    //
    // Same shape as makeObjectTransform above, and that is not a coincidence:
    // SceneUtil::PositionAttitudeTransform::computeLocalToWorldMatrix starts from identity and does
    // preMultTranslate, preMultRotate, preMultScale, which composes to scale * rotate * translate in
    // OSG's row-vector convention -- exactly what makeObjectTransform builds. The two therefore agree
    // element for element on an object that has not moved, and trackableNode below refuses to track
    // anything where they do not.
    //
    // The node's own placement and no ancestor's. Object base nodes hang off a plain osg::Group per
    // cell -- "Cell Root", objects.cpp:49-52 -- so there is no transform above them to accumulate.
    // Accumulating one anyway would be worse than useless: makeObjectTransform ignores ancestors, so
    // any contribution found up there would appear the first time an object moved and the object would
    // jump. This is also why there is no hand-rolled computeLocalToWorld here the way there is in
    // vkparticlereader.cpp -- there is no path to walk, and therefore no CameraRelativeTransform to
    // trip over.
    void makeNodeTransform(const SceneUtil::PositionAttitudeTransform& node, float out[16])
    {
        const osg::Matrixf matrix = osg::Matrixf::scale(node.getScale())
            * osg::Matrixf::rotate(node.getAttitude())
            * osg::Matrixf::translate(node.getPosition());

        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                out[col * 4 + row] = matrix(col, row);
    }

    /// Whether two placements are the same to within float noise.
    ///
    /// Relative rather than absolute. Morrowind's world runs to +/-250,000 units and fp32 has a 24-bit
    /// mantissa, so ulp(250000) is about 1/32 of a world unit: an absolute epsilon tight enough to mean
    /// anything near the origin rejects every object in the east of the map, and one loose enough for
    /// the east of the map accepts a foot of error in an interior.
    bool transformsAgree(const float a[16], const float b[16])
    {
        for (int i = 0; i < 16; ++i)
        {
            const float magnitude = std::max(1.0f, std::max(std::abs(a[i]), std::abs(b[i])));
            if (std::abs(a[i] - b[i]) > magnitude * 1.0e-4f)
                return false;
        }

        return true;
    }

    /// The node this reference's instances may be refreshed from each frame, or null.
    ///
    /// Null is the ordinary answer for a good many references and none of the three cases is an error,
    /// which is why this returns a pointer rather than throwing -- same arrangement as
    /// LandComposite::tryCreate.
    ///
    ///  - No base node. The reference is not in the scene graph at all, so nothing will ever move it.
    ///
    ///  - A base node with no parent. That is object paging's sentinel: scene.cpp:108 keeps *one*
    ///    PositionAttitudeTransform and hands that same pointer to every paged reference
    ///    (scene.cpp:126) without ever adding it to the graph. Its position is the world origin and
    ///    its attitude is whichever paged object happened to be added last, so reading a placement out
    ///    of it would collect every paged static in the cell on the origin, all sharing one rotation.
    ///
    ///  - A node whose placement disagrees with the transform this renderer baked. Then the two
    ///    conventions have drifted apart and there is no way to tell from here which of them is right,
    ///    so the object keeps the placement it already has. That costs a door that will not swing --
    ///    the bug we started with -- and avoids the far worse failure where every tracked object in the
    ///    world jumps somewhere else the first time anything touches it.
    // How deep the TexMat search walks from an object's base node. The animated NIFs are shallow --
    // the controller sits on the NiTriShape or on the NiNode directly above it, under the loaded
    // model's root -- and this is only ever paid by an object addCell already flagged as animated, so
    // it is a bound against a pathological file rather than a performance knob.
    constexpr int sMaxUvDepth = 6;

    // Pull one named node's scrolled UV offset out of the live scene graph.
    //
    // This is the whole of the feature. NifOsg::UVController is a SceneUtil::StateSetUpdater: every
    // frame it evaluates its curves and writes an osg::TexMat into the node's stateset. By the time
    // this reads it, the cubic Hermite interpolation, the phase, the start/stop window and the
    // extrapolation mode have all already been applied by the code that owns them.
    //
    // The stateset is deliberately not cached between frames. StateSetUpdater double-buffers two
    // shallow copies and hands out a different one on alternate frames, so a cached pointer freezes
    // on whichever copy it first caught -- the same hazard vkglowreader documents.
    bool readUvScroll(const osg::Node& node, const std::string& name, int depth, float out[2])
    {
        if (node.getName() == name)
        {
            const osg::StateSet* stateset = node.getStateSet();
            if (stateset == nullptr)
                return false;
            const osg::StateAttribute* attr
                = stateset->getTextureAttribute(0, osg::StateAttribute::TEXMAT);
            const auto* texMat = dynamic_cast<const osg::TexMat*>(attr);
            if (texMat == nullptr)
                return false;
            // UVController::apply already negated U and left V alone when it built this matrix, so
            // the translation comes out in the convention the shaders add directly.
            const osg::Vec3f trans = texMat->getMatrix().getTrans();
            out[0] = trans.x();
            out[1] = trans.y();
            return true;
        }

        if (depth <= 0)
            return false;

        const osg::Group* group = node.asGroup();
        if (group == nullptr)
            return false;
        for (unsigned int i = 0; i < group->getNumChildren(); ++i)
        {
            if (readUvScroll(*group->getChild(i), name, depth - 1, out))
                return true;
        }
        return false;
    }

    const SceneUtil::PositionAttitudeTransform* trackableNode(
        const MWWorld::ConstPtr& ptr, const float bakedTransform[16])
    {
        const SceneUtil::PositionAttitudeTransform* node = ptr.getRefData().getBaseNode();
        if (node == nullptr || node->getNumParents() == 0)
            return nullptr;

        float nodeTransform[16];
        makeNodeTransform(*node, nodeTransform);
        if (!transformsAgree(nodeTransform, bakedTransform))
            return nullptr;

        return node;
    }
}

namespace MWRender
{
    VkRenderingManager::VkRenderingManager(SDL_Window* window, bool enableValidation)
    {
        mRenderer = std::make_unique<Vk::Renderer>(window, enableValidation);
        mMeshConverter
            = std::make_unique<NifVk::MeshConverter>(mRenderer->device(), mRenderer->commandPool());
        // Particle and sky textures resolve through the ordinary loader, so they are cached and
        // evicted with everything else. textureSlot maps a storage index to a live sampler slot; an
        // unloaded texture becomes slot 0, the white fallback, rather than an out-of-range read.
        //
        // One resolver, shared by both readers. The sky finds its images exactly the way the particles
        // do -- by the file name off a stateset -- so a second copy would be a second place to get the
        // "textures/" strip below wrong, and getting it wrong is silent: the texture simply fails to
        // load and everything that wanted it draws as a white square.
        std::function<uint32_t(const std::string&, std::size_t&)> resolveByName
            = [this](const std::string& name, std::size_t& storageIndex) {
            // The leading "textures/" comes off first. These names arrive already resolved through
            // the VFS, whereas getOrLoadTexture expects a raw NIF reference and runs
            // correctTexturePath over it -- which prefixes "textures/" again and produces a path that
            // does not exist. The load then fails silently, textureSlot answers 0, and every flame in
            // the game draws with the 1x1 white fallback. Water hit exactly this and looked the same:
            // a flat white shape where the texture should be.
            constexpr std::string_view prefix = "textures/";
            std::string_view stripped = name;
            if (stripped.size() > prefix.size()
                && Misc::StringUtils::ciEqual(stripped.substr(0, prefix.size()), prefix))
                stripped.remove_prefix(prefix.size());

            const size_t index = getOrLoadTexture(std::string(stripped));
            storageIndex = index;
            const uint32_t slot = static_cast<uint32_t>(textureSlot(index));

            // Slot 0 is the 1x1 white fallback, so a name that lands there draws as a solid white
            // shape and says nothing about why. That has cost hours twice already -- once on the
            // particle textures and once on water -- because a white square looks like a shader bug
            // rather than a missing file.
            //
            // Only after it has failed repeatedly, which is the difference between a real failure and
            // the ordinary way a texture starts life. The first frame that asks for a texture is the
            // frame that queues its upload, so it legitimately answers slot 0 once and resolves on the
            // next -- warning on that made every sky and effect texture in the game report itself
            // broken while they were all working.
            // Consecutive, and the reset is the whole point. Without it this counts *cumulative*
            // failures and eventually accuses a texture that works: every flipbook frame answers slot 0
            // once per cycle, on the frame it first becomes live, because the slot is assigned by the
            // compaction that runs later in the same frame. All 32 enchanted-glow caustics reported
            // themselves broken after about a minute of standing still, and every one of them was fine.
            static std::map<std::string, int, Misc::StringUtils::CiComp> failures;
            if (slot == 0)
            {
                const int count = ++failures[std::string(stripped)];
                if (count == 30)
                    Log(Debug::Warning) << "Vulkan: texture '" << stripped
                                        << "' has answered the white fallback " << count
                                        << " times in a row; it is drawing as a white square";
            }
            else
            {
                failures.erase(std::string(stripped));
            }

            return slot;
        };
        mParticleReader = std::make_unique<ParticleReader>(resolveByName);
        // The device and the command pool are for the two sky meshes, which the reader uploads
        // once, on the first frame it sees them. Constructed here rather than lazily because there is
        // nothing to defer: it allocates nothing until a sky actually turns up, and an interior never
        // gives it one.
        mSkyReader = std::make_unique<SkyReader>(
            resolveByName, mRenderer->device(), mRenderer->commandPool());
        // The same resolver again, for the 32 caustic frames the glow cycles through. They are
        // ordinary textures and there is no reason for them to be loaded, cached or evicted on any
        // other terms.
        mGlowReader = std::make_unique<GlowReader>(resolveByName);
        Log(Debug::Info) << "Vulkan renderer initialized";
    }

    VkRenderingManager::~VkRenderingManager() = default;

    bool VkRenderingManager::loadShaders(const std::filesystem::path& shaderDir)
    {
        // Built here rather than in the constructor because it needs the shader directory, and kept
        // out of the return value on purpose: a renderer that cannot composite land textures still
        // draws terrain, just with the hard tile edges it had before. Losing the whole renderer over
        // a cosmetic pass would be the wrong trade.
        mLandComposite
            = LandComposite::tryCreate(mRenderer->device(), mRenderer->commandPool(), shaderDir.string());
        if (mLandComposite == nullptr)
            Log(Debug::Warning) << "Vulkan: no land composite shaders in " << shaderDir
                                << "; terrain will have hard tile edges";

        return mRenderer->loadShadersAndCreatePipelines(shaderDir.string());
    }

    static void osgMatrixToMat4(const osg::Matrixf& m, Vk::Mat4& out)
    {
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                out.data[col * 4 + row] = m(col, row);
    }

    namespace
    {
        // Vk::srgbToLinear does the work; see vkmath.hpp for why every authored colour needs it and
        // why scalars must not have it.
        Vk::Vec4 decodeColor(const osg::Vec4f& c)
        {
            return { Vk::srgbToLinear(c.r()), Vk::srgbToLinear(c.g()), Vk::srgbToLinear(c.b()), c.a() };
        }
    }

    void VkRenderingManager::render(Camera& camera, const FrameLighting& lighting)
    {
        const osg::Vec3f& sunLightDir = lighting.sunLightDir;
        Vk::SceneData scene = {};

        const auto& viewMatrix = camera.getViewMatrix();
        const auto& projMatrix = camera.getProjectionMatrix();

        osgMatrixToMat4(viewMatrix, scene.view);
        osgMatrixToMat4(projMatrix, scene.projection);

        // OSG hands us an OpenGL-convention projection (Y up, depth [-1, 1]). The Vulkan pipeline
        // uses a standard viewport (Y down, depth [0, 1]), so the matrix has to be corrected before
        // it is used or inverted -- raygen.rgen reconstructs world positions from projInverse and a
        // [0, 1] depth buffer, so both must agree on the convention.
        scene.projection = Vk::glToVulkanProjection(scene.projection);

        scene.viewInverse = Vk::invertMat4(scene.view);
        scene.projInverse = Vk::invertMat4(scene.projection);

        // Carried across frames so the temporal accumulator can find where a surface was last frame.
        //
        // This is prevView * inverse(curView), not the previous frame's projection * view. The
        // reasoning is on Vk::SceneData::prevViewFromCurView and it is specific to this world: at
        // Morrowind's +/-250,000-unit scale, reprojecting through a previous view-projection multiplies
        // two quantities of that magnitude together to produce one of magnitude ~50, and fp32 has no
        // headroom for it. Composing view-to-view instead keeps every operand small -- the rotation
        // block is a product of two rotations and the translation is a single frame of camera motion.
        //
        // Identity on the first frame rather than garbage. mFrameIndex == 0 is belt to the braces
        // though; the real gate is the per-pixel history length, which the accumulator's images are
        // cleared to zero for exactly this reason.
        if (mFrameIndex == 0)
            Vk::identityMat4(scene.prevViewFromCurView.data);
        else
            Vk::multiplyMat4(mPrevView.data, scene.viewInverse.data, scene.prevViewFromCurView.data);
        scene.frameIndex = mFrameIndex;
        mPrevView = scene.view;
        ++mFrameIndex;

        // Temporal accumulator tuning. See Vk::SceneData::denoiseParams for the channel assignment.
        //
        // The depth tolerance is relative and the shader adds a 1/NdotV slope term on top of it,
        // which is not a fudge factor: Morrowind's heightfield is 128 units between vertices and is
        // almost always seen at grazing angles, where a single ground pixel spans hundreds of units of
        // depth between its corners. A flat relative tolerance rejects the entire ground plane the
        // moment the camera moves, which is where the accumulation matters most.
        //
        // The normal threshold is 0.9 -- about 25 degrees -- rather than something tighter. Terrain
        // normals are interpolated per-vertex across those 128-unit quads and adjacent pixels
        // routinely differ by several degrees on ground that is perfectly continuous, so a tight
        // threshold reintroduces noise in a visible grid. 0.9 still rejects the hard creases of
        // Morrowind's low-poly object meshes, where adjacent faces differ by 60 degrees or more.
        scene.denoiseParams = { 1.0f / 16.0f, 0.01f, 0.9f, 32.0f };

        // The sun's angular radius, which is what makes the shadow term stochastic and is therefore
        // the first real consumer of the accumulator above.
        //
        // Deliberately far larger than the real sun's ~0.27 degrees. Two reasons: at Morrowind's
        // scale the true figure reads as almost perfectly hard, and the renderer has no ambient
        // occlusion and no GI bounce, so a wider penumbra also stands in for contact softening that
        // nothing else supplies. Tune by eye; setting it to zero restores the previous hard shadow
        // exactly, which is what makes the old behaviour a usable reference for this one.
        constexpr float sSunAngularRadiusDegrees = 0.75f;
        constexpr float sDegToRad = 3.14159265358979f / 180.0f;
        scene.sunParams = { std::cos(sSunAngularRadiusDegrees * sDegToRad), 0.0f, 0.0f, 0.0f };

        // The water plane, in the three spare floats of sunParams. See Vk::SceneData::sunParams for
        // why it went there and not into a field of its own.
        //
        // The clock is the only one this renderer has. frameIndex counts frames and says nothing
        // about how long they took, and there is no time value anywhere else in SceneData -- so
        // without this the waves cannot move, which is most of why the old surface read as a sheet.
        mWaterSeconds += MWBase::Environment::get().getFrameDuration();

        // Taken from the player's cell rather than from the loaded cell set. An exterior loads nine
        // cells that all share sea level and an interior loads one, so the set never disagrees with
        // itself -- but the question being asked is which body of water the camera is standing in,
        // and that is a property of where the player is.
        float waterHeight = 0.0f;
        bool hasWater = false;
        {
            const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
            if (!player.isEmpty() && player.isInCell() && player.getCell()->getCell() != nullptr)
            {
                // Exteriors always have water: MWWorld::Cell forces hasWater on and the height to -1
                // for them (cell.cpp:100-101), which is Morrowind's sea level.
                const MWWorld::Cell* cell = player.getCell()->getCell();
                hasWater = cell->hasWater();
                waterHeight = cell->getWaterHeight();
            }
        }

        // Only if the normal map actually made it to the device. This flag gates the water draw *and*
        // the particle pass's underwater attenuation, and a torch dimmed by water that is not being
        // drawn is a worse failure than one that is not dimmed at all.
        const uint32_t waterNormalSlot = textureSlot(mWaterNormalTexture);
        hasWater = hasWater && waterNormalSlot != 0u;

        scene.sunParams.y = static_cast<float>(mWaterSeconds);
        scene.sunParams.z = waterHeight;
        scene.sunParams.w = hasWater ? 1.0f : 0.0f;
        // In the uniform block rather than a push constant on the water pipeline, because raygen.rgen
        // builds the same wave normal to aim the reflection ray and a raygen shader has no push
        // constant range to receive it in. Zero when the texture is not resident, which is the same
        // condition that just cleared sunParams.w -- and it is sunParams.w that suppresses the draw.
        scene.waterNormalMap = waterNormalSlot;

        // The shaders reconstruct the direction *towards* the light as -sunDirection, so this has to be
        // the direction the light travels. World::getSunLightPosition() is the opposite convention -- it
        // points towards the sun -- and the caller negates it. Getting this backwards leaves every
        // up-facing surface with NdotL == 0, lit by ambient alone, and sends every shadow ray straight
        // into the ground.
        osg::Vec3f lightDir = sunLightDir;
        if (lightDir.normalize() == 0.0f)
            lightDir = osg::Vec3f(0.0f, 0.0f, -1.0f);
        scene.sunDirection = { lightDir.x(), lightDir.y(), lightDir.z(), 0.0f };
        // Both come off the light the OSG renderer uses, so they already carry the cell's authored
        // mood colour, the minimum interior brightness floor, and the weather system's time-of-day sun
        // colour. Reading them rather than recomputing is the whole reason a cell in a Dwemer ruin
        // looks different from one in an Ashlander yurt.
        scene.sunColor = decodeColor(lighting.sunDiffuse);
        scene.ambientColor = decodeColor(lighting.ambient);
        scene.isInterior = lighting.isInterior ? 1u : 0u;
        scene.skyColor = decodeColor(lighting.skyColour);
        scene.fogColor = decodeColor(lighting.fogColour);
        // Distances, not colours -- decoding these would be meaningless.
        scene.fogParams = { lighting.fogStart, lighting.fogEnd, 0.0f, 0.0f };

        // Before updateScene: it is what stamps the light count into the scene data.
        // VkPointLight and Vk::PointLight are the same 64-byte layout, pinned by static_asserts on
        // both sides, so this reinterpret is safe -- components cannot depend on apps, which is why
        // the struct is declared twice rather than shared.
        if (lighting.pointLights != nullptr && !lighting.pointLights->empty())
        {
            static_assert(sizeof(VkPointLight) == sizeof(Vk::PointLight),
                "the light structs must agree or the buffer upload reinterprets garbage");
            mRenderer->updateLights(reinterpret_cast<const Vk::PointLight*>(lighting.pointLights->data()),
                static_cast<uint32_t>(lighting.pointLights->size()));
        }
        else
        {
            mRenderer->updateLights(nullptr, 0);
        }

        // Gathered in syncCells, which runs earlier in the same frame and is where a newly seen
        // particle texture can still be given a sampler slot. Sorted here, because the camera is not
        // known until now.
        if (mParticleReader != nullptr)
        {
            mParticleReader->sortForCamera(camera.getPosition());
            const std::vector<Vk::ParticleQuad>& quads = mParticleReader->quads();
            mRenderer->updateParticles(
                quads.data(), static_cast<uint32_t>(quads.size()), mParticleReader->runs());
        }

        // Gathered in syncCells for the same reason and handed over unsorted: three quads that never
        // overlap need no depth order, and the two moons cross the sun only when the sky has already
        // faded them out.
        // The sun flash, the sun glare and the disc the visibility test traces against.
        //
        // Everything except the occlusion is worked out here, on the CPU, and the draw is
        // dropped outright when the answer is already zero. That is not an optimisation bolted
        // on -- it is what SunGlareCallback and SunFlashCallback themselves do, returning
        // without traversing when the fade or the scale comes out at zero (skyutil.cpp lines
        // 201-205 and 277-281). It is also what keeps a fullscreen additive pass off the bill
        // at night, indoors, and any time the sun is more than thirty degrees off the view
        // axis, which between them is most of the game.
        {
            // The three ini constants, read once. SunGlareCallback reads exactly these three in
            // its constructor (skyutil.cpp lines 249-251).
            //
            // From the Fallback map rather than off the graph, and that is not a shortcut. The
            // glare's colour and its fade never reach the scene graph at all: the callback keeps
            // them in private members and writes them onto a stateset it pushes at cull time
            // (line 287), which a node visitor cannot see. Every other number in this renderer's
            // sky is read off the graph because it is there to be read; these are not there.
            static const osg::Vec4f sGlareColour = [] {
                osg::Vec4f colour = Fallback::Map::getColour("Weather_Sun_Glare_Fader_Color");
                // Replicating a design flaw in MW, in the words of the comment this copies. The
                // colour was set on both the ambient and the emissive property, which doubles
                // it, and the fixed function pipeline then clamped the result. With the stock
                // ini only the red component clamps, so the wash comes out orange rather than
                // red. Drop the doubling and the sun glare is a different colour from the one
                // Morrowind has had since 2002.
                colour *= 2;
                for (int i = 0; i < 3; ++i)
                    colour[i] = std::min(1.f, colour[i]);
                return colour;
            }();
            static const float sGlareFaderMax = Fallback::Map::getFloat("Weather_Sun_Glare_Fader_Max");
            static const float sGlareAngleMax = Fallback::Map::getFloat("Weather_Sun_Glare_Fader_Angle_Max");

            Vk::SunVisibility disc = {};
            const Vk::SkyElement* flash = nullptr;
            Vk::SkyElement glare = {};
            bool hasGlare = false;

            if (mSkyReader != nullptr && mSkyReader->sunDiscTanRadius() > 0.f)
            {
                const osg::Vec3f& sunDir = mSkyReader->sunDiscDirection();
                disc.discDir[0] = sunDir.x();
                disc.discDir[1] = sunDir.y();
                disc.discDir[2] = sunDir.z();
                disc.discDir[3] = mSkyReader->sunDiscTanRadius();
                disc.params[0] = mSkyReader->sunDiscTexture();
                // Sixty-four rays: one RDNA2 wave, and one stratified sample per cell of the
                // 8x8 grid raygen lays over the disc. Finer than the thresholds that read it --
                // the flash's fade band is the bottom tenth, which is six of these -- and
                // coarse enough that the whole test is lost in the noise of a frame that already
                // traces a million shadow rays. Changing it means changing sGridSide in
                // raygen.rgen to match; the two are one number written twice.
                disc.params[1] = 64;

                flash = mSkyReader->sunFlash();

                // The angle between the view direction and the sun, exactly as
                // SunGlareCallback::getAngleToSunInRadians takes it (skyutil.cpp lines 298-310):
                // out of the view matrix by getLookAt, against the sun transform's own position.
                // Taken from the same osg::Matrixf the renderer builds scene.view from, so there
                // is no second convention here to get backwards -- and getting it backwards
                // gives a glare that peaks when the sun is behind you, which looks like a
                // brightness bug rather than a sign error.
                osg::Vec3d eye, centre, up;
                camera.getViewMatrix().getLookAt(eye, centre, up);
                osg::Vec3d forward = centre - eye;
                forward.normalize();
                osg::Vec3d sun(sunDir.x(), sunDir.y(), sunDir.z());
                const float angleRadians
                    = static_cast<float>(std::acos(std::clamp(forward * sun, -1.0, 1.0)));

                const float angleMaxRadians = osg::DegreesToRadians(sGlareAngleMax);
                const float value = 1.f - std::min(1.f, angleRadians / angleMaxRadians);
                // The three factors that are not the occlusion. mGlareView is the sun's own
                // material alpha, read off the graph; mTimeOfDayFade came in on FrameLighting
                // because there is nowhere on the graph it exists. The fourth factor, the
                // visible ratio, is applied in the shader from a value that will not exist until
                // the ray tracing pass has run.
                const float fade = value * sGlareFaderMax
                    * lighting.sunGlareTimeOfDayFade * mSkyReader->sunGlareView();

                if (fade > 0.f)
                {
                    glare.params[2] = static_cast<uint32_t>(Vk::SkyMode::SunGlare);
                    // Gamma, on purpose, and sky.frag depends on it. paintSunglare multiplies
                    // this colour by the fade in gamma space against a gamma framebuffer, and
                    // the product does not commute with the transfer function -- decoding here
                    // and scaling the linear value there is a visibly different colour, not a
                    // rounding difference. The one place in this renderer a colour is handed to
                    // a shader undecoded, and the reason is written out at the branch that
                    // consumes it.
                    glare.colour[0] = sGlareColour.r();
                    glare.colour[1] = sGlareColour.g();
                    glare.colour[2] = sGlareColour.b();
                    glare.colour[3] = fade;
                    hasGlare = true;
                }
            }

            mRenderer->updateSunGlare(flash, hasGlare ? &glare : nullptr, disc);
        }

        if (mSkyReader != nullptr)
        {
            mRenderer->updateSky(mSkyReader->elements());
            // The cloud layer and the night sky, in the same handover. Kept in graph order rather than
            // sorted: each one already carries the flag that says which side of the sun and the moons
            // it belongs on, which is the only ordering that matters and is not something a distance
            // sort could recover -- the two cloud layers of a weather crossfade are the same mesh at
            // the same distance.
            mRenderer->updateSkyMeshes(mSkyReader->meshes());
        }

        mRenderer->updateScene(scene);

        // One frustum per frame, from the same matrix the vertex shader uses, so what is culled and
        // what is drawn cannot disagree.
        Vk::Mat4 viewProjection;
        Vk::multiplyMat4(scene.projection.data, scene.view.data, viewProjection.data);
        const Vk::Frustum frustum = Vk::extractFrustum(viewProjection);
        size_t drawn = 0;
        size_t culled = 0;

        for (const auto& [store, cellMeshes] : mCellMeshes)
        {
            for (const auto& inst : cellMeshes.instances)
            {
                const auto& mesh = mMeshes[inst.meshIndex];
                Vk::Mat4 transform;
                std::memcpy(transform.data, inst.transform, sizeof(float) * 16);

                const size_t textureIndex = mMeshTextures[inst.meshIndex];

                // Frustum cull. The mesh's bounds are object space, so they go through the same
                // instance transform the geometry does. This gates rasterization only -- the
                // submission still enters the TLAS, because an object behind the camera casts a
                // shadow into view and a reflection ray can hit anything.
                float worldMin[3];
                float worldMax[3];
                Vk::transformBounds(transform, mesh->boundsMin, mesh->boundsMax, worldMin, worldMax);
                const bool visible = Vk::boxInFrustum(frustum, worldMin, worldMax);
                if (visible)
                    ++drawn;
                else
                    ++culled;

                Vk::MeshSubmission submission;
                submission.vertexBuffer = mesh->vertexBuffer->handle();
                submission.indexBuffer = mesh->indexBuffer->handle();
                submission.indexCount = mesh->indexCount;
                submission.transform = transform;
                // Zero once this object has been seen to move, exactly as the actors below are
                // zero, and for the same reasons: buildTlas idles the device to retire the old
                // structure, and the denoiser's reprojection is exact only while every instance in the
                // TLAS is static. An opened door therefore stops casting a ray traced shadow and stops
                // appearing in reflections. That is the price of this fix and it is a deliberate one --
                // see CellMeshes::Instance::traced.
                submission.blasAddress = (inst.traced && mesh->blas)
                    ? mesh->blas->deviceAddress()
                    : VkDeviceAddress{ 0 };
                submission.vertexAddress = mesh->vertexBuffer->deviceAddress();
                submission.indexAddress = mesh->indexBuffer->deviceAddress();
                // The sampler slot, not the storage index. Since eviction landed the two are
                // different: mTextureSlots holds 0 -- the white fallback -- for any texture no loaded
                // cell references, and a compacted slot for the ones they do.
                submission.textureIndex = textureSlot(textureIndex);
                submission.alphaTested = mesh->alphaTested;
                // How the shape was authored to be composited. The renderer decides what it can do
                // with each of these; see NifVk::MeshRenderState for what the content contains.
                submission.alphaTest = mesh->renderState.alphaTest;
                submission.alphaFunc = mesh->renderState.alphaFunc;
                submission.alphaThreshold = mesh->renderState.alphaThreshold;
                submission.twoSided = mesh->renderState.twoSided;
                submission.additive = mesh->renderState.additive();
                submission.roughness = mesh->roughness;
                submission.specularStrength = mesh->specularStrength;
                submission.visible = visible;
                // Enchanted glow, read off the live object this frame. Zero for the overwhelming
                // majority -- 553 enchanted references exist across the whole game -- and the renderer
                // skips a submission whose glow texture is zero, so this costs four stores.
                submission.glowColour[0] = inst.glowColour[0];
                submission.glowColour[1] = inst.glowColour[1];
                submission.glowColour[2] = inst.glowColour[2];
                submission.glowTexture = inst.glowTexture;

                // Scrolling texture -- lava, a waterfall, a Ghostgate fence. A slot is taken only for
                // the shapes that need one, so the table stays the length of what is actually moving
                // on screen rather than the length of the draw list.
                if (!mesh->uvControllerNode.empty() && mUvScrollTable.size() < Vk::maxUvScrolls)
                {
                    submission.uvScroll = static_cast<uint32_t>(mUvScrollTable.size());
                    mUvScrollTable.push_back({ inst.uvScroll[0], inst.uvScroll[1] });
                }

                mRenderer->submitMesh(submission);
            }
        }

        // Before the first skinned submission, because it is what decides whether a palette is really
        // there to be pointed at.
        const uint32_t uploadedMatrices = mRenderer->updateSkinMatrices(
            mSkinMatrices.data(), static_cast<uint32_t>(mSkinMatrices.size() / 16));

        for (const auto& inst : mActorInstances)
        {
            const auto& mesh = mMeshes[inst.meshIndex];
            Vk::Mat4 transform;
            std::memcpy(transform.data, inst.transform, sizeof(float) * 16);

            const bool isSkinned = inst.boneOffset != Vk::sNoBones && mesh->skinBuffer
                && inst.boneOffset + mesh->skinBones.size() <= uploadedMatrices;

            float worldMin[3];
            float worldMax[3];
            if (isSkinned)
            {
                // Posed bounds, computed with the palette in syncActors. The mesh's own bounds
                // describe where the vertices sit *before* the pose and culling on those threw away
                // all but one skinned shape a frame -- HANDOFF trap 37.
                std::copy(inst.worldMin, inst.worldMin + 3, worldMin);
                std::copy(inst.worldMax, inst.worldMax + 3, worldMax);
            }
            else
            {
                Vk::transformBounds(transform, mesh->boundsMin, mesh->boundsMax, worldMin, worldMax);
            }
            const bool visible = Vk::boxInFrustum(frustum, worldMin, worldMax);
            if (visible)
                ++drawn;
            else
                ++culled;

            Vk::MeshSubmission submission;
            submission.vertexBuffer = mesh->vertexBuffer->handle();
            submission.indexBuffer = mesh->indexBuffer->handle();
            submission.indexCount = mesh->indexCount;
            submission.transform = transform;
            // Deliberately not in the acceleration structure, and this is the whole reason actors
            // can move at all today. buildTlas idles the device and allocates a new structure, and
            // markTlasDirty only fires on cell load; something that moves every frame would mean
            // rebuilding every frame, which is a device stall per frame. It would also break the
            // denoiser's reprojection, which is exact rather than approximate precisely because the
            // acceleration structure is static (vkrenderer.hpp, and trap 22). So actors rasterise
            // into the G-buffer and are invisible to every ray: they receive no ray traced shadow
            // and cast none. Fixing that is the motion vector work in DENOISER-PLAN section 7.
            submission.blasAddress = VkDeviceAddress{ 0 };
            submission.vertexAddress = mesh->vertexBuffer->deviceAddress();
            submission.indexAddress = mesh->indexBuffer->deviceAddress();
            submission.textureIndex = textureSlot(mMeshTextures[inst.meshIndex]);
            submission.alphaTested = mesh->alphaTested;
            // As above. Actors carry these too: a Golden Saint's aura and a summon's effect shell are
            // additive shapes hung on a skeleton, and body parts are the one place a mod's
            // two-sided cloth actually turns up.
            submission.alphaTest = mesh->renderState.alphaTest;
            submission.alphaFunc = mesh->renderState.alphaFunc;
            submission.alphaThreshold = mesh->renderState.alphaThreshold;
            submission.twoSided = mesh->renderState.twoSided;
            submission.additive = mesh->renderState.additive();
            submission.roughness = mesh->roughness;
            submission.specularStrength = mesh->specularStrength;
            submission.visible = visible;
            // Only if the palette actually reached the device. uploadedMatrices is what the renderer
            // took, which is less than what was offered when the frame overflowed, and a shape whose
            // palette fell off the end would otherwise read whatever is at that offset -- another
            // actor's bones, or last frame's.
            if (isSkinned)
            {
                submission.skinBuffer = mesh->skinBuffer->handle();
                submission.boneOffset = inst.boneOffset;
            }

            mRenderer->submitMesh(submission);
        }

        for (const auto& [store, terrain] : mCellTerrain)
        {
            Vk::Mat4 transform;
            std::memcpy(transform.data, terrain.transform, sizeof(float) * 16);

            for (const auto& chunk : terrain.chunks)
            {
                Vk::MeshSubmission submission;
                submission.vertexBuffer = chunk.geometry.vertexBuffer->handle();
                submission.indexBuffer = chunk.geometry.indexBuffer->handle();
                submission.indexCount = chunk.geometry.indexCount;
                submission.transform = transform;
                submission.blasAddress = chunk.geometry.blasAddress();
                submission.vertexAddress = chunk.geometry.vertexAddress();
                submission.indexAddress = chunk.geometry.indexAddress();
                submission.textureIndex = textureSlot(chunk.textureIndex);
                // Terrain is a solid heightfield; leaving it opaque keeps the fast traversal path.
                submission.alphaTested = false;
                // Dirt and rock. No specular, which is also what the OSG renderer gives terrain.
                submission.roughness = 1.0f;
                submission.specularStrength = 0.0f;

                mRenderer->submitMesh(submission);
            }
        }

        // Logged once, not per frame: the point is to confirm the cull is actually doing something
        // and to catch the failure mode where a wrong frustum culls everything or nothing.
        if (!mLoggedCullRatio && drawn + culled > 0)
        {
            mLoggedCullRatio = true;
            Log(Debug::Info) << "Vulkan: frustum culling drew " << drawn << " of " << (drawn + culled)
                             << " object instances, of which " << mActorInstances.size()
                             << " are actor meshes";
        }

        mRenderer->updateUvScroll(mUvScrollTable.data(), static_cast<uint32_t>(mUvScrollTable.size()));
        // Reset here rather than at the top of the next frame so the table's whole lifetime is inside
        // one function: everything that fills it is above, and nothing outside render() can be handed
        // a stale entry. The zero at index 0 is what every non-scrolling shape reads.
        mUvScrollTable.clear();
        mUvScrollTable.push_back({ 0.0f, 0.0f });

        mRenderer->render();
    }

    void VkRenderingManager::addCell(const MWWorld::CellStore* store)
    {
        if (store == nullptr || mCellMeshes.find(store) != mCellMeshes.end())
            return;

        CellMeshes cellMeshes;
        size_t skipped = 0;
        size_t untracked = 0;

        store->forEachConst([&](const MWWorld::ConstPtr& ptr) {
            // Actors are handled by syncActors instead. Their transform has to be recomputed every
            // frame, and an instance baked here would be a second, motionless copy of the same
            // creature standing where it happened to be when the cell loaded.
            if (ptr.getClass().isActor())
                return true;

            VFS::Path::Normalized model;
            try
            {
                model = ptr.getClass().getCorrectedModel(ptr);
            }
            catch (const std::exception&)
            {
                return true;
            }

            if (model.empty())
                return true;

            const std::vector<size_t>* meshIndices = getOrLoadMeshes(std::string(model.value()));
            if (meshIndices == nullptr || meshIndices->empty())
            {
                ++skipped;
                return true;
            }

            float objectTransform[16];
            makeObjectTransform(ptr, objectTransform);

            const uint32_t firstInstance = static_cast<uint32_t>(cellMeshes.instances.size());

            for (size_t meshIndex : *meshIndices)
            {
                CellMeshes::Instance instance;
                instance.meshIndex = meshIndex;
                // mMeshPlacements already holds this submesh's offset from the object origin --
                // its place in the NIF node hierarchy, or its posed bone for a skinned shape --
                // so the final instance transform is object * that.
                Vk::multiplyMat4(objectTransform, mMeshPlacements[meshIndex].data(), instance.transform);
                cellMeshes.instances.push_back(instance);
            }

            // Remember where to read this object's placement back from, so refreshMovedObjects can
            // rewrite those instances when a script, a door state or the physics settles it somewhere
            // else. Without this the transform above is the only one the object ever gets, which is
            // why nothing in the world except an actor could move.
            if (const SceneUtil::PositionAttitudeTransform* node = trackableNode(ptr, objectTransform))
            {
                CellMeshes::MovedObject tracked;
                tracked.node = node;
                for (size_t meshIndex : *meshIndices)
                {
                    if (!mMeshes[meshIndex]->uvControllerNode.empty())
                    {
                        tracked.uvAnimated = true;
                        break;
                    }
                }
                tracked.position = node->getPosition();
                tracked.attitude = node->getAttitude();
                tracked.scale = node->getScale();
                tracked.firstInstance = firstInstance;
                tracked.instanceCount
                    = static_cast<uint32_t>(cellMeshes.instances.size()) - firstInstance;
                cellMeshes.tracked.push_back(std::move(tracked));
            }
            else
            {
                ++untracked;
            }

            return true;
        });

        size_t textured = 0;
        for (const auto& mesh : mMeshes)
        {
            if (!mesh->baseTexture.empty())
                ++textured;
        }

        // untracked is worth a number rather than silence. A cell where it is roughly the whole
        // reference count means trackableNode is rejecting everything, which is what a broken
        // placement convention looks like from the outside -- a world that draws correctly and in
        // which nothing can ever move again.
        Log(Debug::Info) << "Vulkan: loaded cell with " << cellMeshes.instances.size()
                         << " instances (" << skipped << " models skipped, "
                         << cellMeshes.tracked.size() << " objects tracked, " << untracked
                         << " not), "
                         << mMeshes.size() << " meshes cached total, "
                         << textured << " with a base texture, "
                         << mTextures.size() << " textures uploaded";

        mCellMeshes.emplace(store, std::move(cellMeshes));

        addTerrain(store);

        // Deliberately no syncTexturesToRenderer() here. The sampler array is now rebuilt from what
        // the loaded cells reference, so it can only be rebuilt once this cell is in mCellMeshes and
        // its terrain exists -- and the only caller, syncCells, does exactly that once for the whole
        // batch. Syncing here as well would rewrite the descriptor array behind a vkDeviceWaitIdle
        // once per cell added, which is three stalls per grid crossing instead of one.
    }

    void VkRenderingManager::addTerrain(const MWWorld::CellStore* store)
    {
        if (store == nullptr || mCellTerrain.find(store) != mCellTerrain.end())
            return;

        const MWWorld::Cell* cell = store->getCell();
        if (cell == nullptr)
            return;

        CellTerrain terrain;
        Vk::identityMat4(terrain.transform);

        std::vector<LandChunk> landChunks;
        size_t compositeTexture = sNoTexture;
        if (cell->isExterior())
        {
            const int cellX = cell->getGridX();
            const int cellY = cell->getGridY();

            // Blended ground where the cell has more than one land texture, hard-edged tiles where
            // it does not or where the bake is unavailable. The two paths need different geometry --
            // one chunk with 0..1 UVs against several chunks with 0..16 -- so the blend is resolved
            // before the chunks are built rather than after.
            const LandBlend blend = buildLandBlend(cellX, cellY);
            compositeTexture = bakeLandComposite(blend);

            landChunks = buildLandChunks(cellX, cellY, compositeTexture != sNoTexture);

            // Vertices come out cell-local, so all that is left is the translation to the cell's
            // south-west corner. Column-vector convention, matching makeObjectTransform.
            terrain.transform[12] = static_cast<float>(cellX) * ESM::Land::REAL_SIZE;
            terrain.transform[13] = static_cast<float>(cellY) * ESM::Land::REAL_SIZE;
        }
        // An interior has no heightfield and no cell offset, and now nothing else either. It used to
        // reach addWater below -- Morrowind's caves and canalworks have water in them -- which is why
        // this did not simply return early. The surface is one camera-following grid drawn in the
        // composite pass now, so an interior falls through the empty check below and registers no
        // terrain at all, which is correct: there is none.

        size_t triangles = 0;

        for (const LandChunk& chunk : landChunks)
        {
            const uint32_t vertexCount = static_cast<uint32_t>(chunk.vertices.size() / Vk::sFloatsPerVertex);
            const uint32_t indexCount = static_cast<uint32_t>(chunk.indices.size());

            Vk::Geometry geometry = Vk::uploadGeometry(mRenderer->device(), mRenderer->commandPool(),
                chunk.vertices.data(), vertexCount, chunk.indices.data(), indexCount, false);
            if (!geometry.valid())
                continue;

            CellTerrain::Chunk uploaded;
            uploaded.geometry = std::move(geometry);
            // A composited chunk carries no texture name of its own -- there is no single land texture
            // that describes it -- so the baked slot is used directly.
            uploaded.textureIndex
                = compositeTexture != sNoTexture ? compositeTexture : getOrLoadTexture(chunk.texture);
            terrain.chunks.push_back(std::move(uploaded));

            triangles += indexCount / 3;
        }

        if (terrain.chunks.empty())
            return;

        syncTexturesToRenderer();

        Log(Debug::Info) << "Vulkan: loaded terrain for " << cell->getDescription() << " -- "
                         << terrain.chunks.size() << " chunks, " << triangles << " triangles";

        mCellTerrain.emplace(store, std::move(terrain));
    }

    void VkRenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mCellMeshes.erase(store);
        mCellTerrain.erase(store);

        // Nothing calls this today -- syncCells erases from the maps directly -- but if anything ever
        // does, the sampler array has to be rebuilt or the unloaded cell's textures stay resident and
        // keep their slots, which is the growth that made the array overflow in the first place.
        syncTexturesToRenderer();
    }

    void VkRenderingManager::syncCells(const std::set<MWWorld::CellStore*, std::less<>>& activeCells)
    {
        // Active cell counts are small (a 3x3 exterior grid at most), so linear lookups are cheaper
        // than building a lookup structure every frame.
        const auto isActive = [&](const MWWorld::CellStore* cell) {
            for (const MWWorld::CellStore* active : activeCells)
            {
                if (active == cell)
                    return true;
            }
            return false;
        };

        const auto anyInactive = [&](const auto& map) {
            for (const auto& entry : map)
            {
                if (!isActive(entry.first))
                    return true;
            }
            return false;
        };

        // Erasing a CellTerrain runs Vk::Geometry's destructor, which destroys vertex/index buffers and
        // a BLAS immediately. Up to maxFramesInFlight command buffers may still reference them, and the
        // live TLAS still holds device addresses of those BLASes, so destroying them here without
        // waiting is a use-after-free that can surface as VK_ERROR_DEVICE_LOST when crossing a cell
        // boundary. Cell transitions are rare enough that an idle is acceptable; the scalable answer is
        // a per-frame retirement list that frees a resource maxFramesInFlight frames after its last use.
        if (anyInactive(mCellTerrain) || anyInactive(mCellMeshes))
            mRenderer->waitIdle();

        bool changed = false;

        for (auto it = mCellMeshes.begin(); it != mCellMeshes.end();)
        {
            if (isActive(it->first))
            {
                ++it;
            }
            else
            {
                it = mCellMeshes.erase(it);
                changed = true;
            }
        }

        for (auto it = mCellTerrain.begin(); it != mCellTerrain.end();)
        {
            if (isActive(it->first))
            {
                ++it;
            }
            else
            {
                it = mCellTerrain.erase(it);
                // Terrain holds texture indices of its own, so dropping a chunk can free a land
                // texture even when no object mesh changed.
                changed = true;
            }
        }

        for (MWWorld::CellStore* cell : activeCells)
        {
            if (mCellMeshes.find(cell) != mCellMeshes.end())
                continue;
            addCell(cell);
            changed = true;
        }

        // Before the texture sync, not after: collectLiveTextures walks mActorInstances, and a
        // texture only an actor references would otherwise be evicted and come back as the white
        // fallback -- which for alpha-tested geometry is worse than a missing texture (trap 13).
        syncActors(activeCells);

        // Before the texture sync for the same reason syncActors is. A particle texture is first
        // seen here, and getOrLoadTexture only puts it in mTextures -- textureSlot answers 0, the
        // white fallback, until syncTexturesToRenderer has rebuilt the sampler array. Collecting
        // after the sync meant every flame in the game drew as a solid white quad, which looks like
        // a texture that failed to load rather than one that loaded a moment too late.
        if (mParticleReader != nullptr)
        {
            const size_t texturesBefore = mTextures.size();
            mParticleReader->collect(mSceneRoot);
            if (mTextures.size() != texturesBefore)
                changed = true;
        }

        // And the sky, for the same reason and in the same window. A moon changes phase at midnight
        // and the new image is first seen here; collecting after the texture sync would draw it as the
        // white fallback for a frame, which in the sky is the brightest thing on screen.
        if (mSkyReader != nullptr)
        {
            const size_t texturesBefore = mTextures.size();
            mSkyReader->collect(mSceneRoot);
            if (mTextures.size() != texturesBefore)
                changed = true;
        }

        // Loaded once, and here rather than in the constructor because it goes through the ordinary
        // texture path -- which needs the resource system, and that is not up when this object is
        // built. Ahead of the sync below so it is given a sampler slot in the same pass; a frame with
        // the texture loaded but no slot draws the sea with the white fallback, which is the flat
        // untextured sheet this whole change exists to get rid of.
        if (!mWaterNormalRequested)
        {
            mWaterNormalRequested = true;
            // UNORM, not sRGB. See toVkFormat: these texels are directions, not colour.
            mWaterNormalTexture = getOrLoadTexture("omw/water_nm.png", false);
            if (mWaterNormalTexture == sNoTexture)
            {
                Log(Debug::Warning) << "Vulkan: textures/omw/water_nm.png could not be loaded; "
                                       "the water surface will not be drawn";
            }
            changed = true;
        }

        // Once, after every add and erase, rather than per cell. This is the path that actually
        // unloads cells -- the loops above erase from the maps directly rather than going through
        // removeCell -- and each call rewrites the descriptor array behind a vkDeviceWaitIdle, so
        // doing it per cell would stall several times per grid crossing for no benefit.
        if (changed)
            syncTexturesToRenderer();

        // After the adds, so a cell loaded on this very frame is compared against the placement it
        // was just baked with rather than sitting still for one more frame.
        //
        // This is also the right side of osgViewer's update traversal. Engine::frame runs
        // updateTraversal at engine.cpp:356 and calls syncCells at engine.cpp:394, so a door that
        // World::processDoors swung this frame has already had setAttitude called on its node by the
        // time this reads it, and the Vulkan image agrees with the OSG one on the same frame rather
        // than trailing it by one.
        // Before refreshMovedObjects, which is where the glow is actually read. All this
        // clears is the "did anything glow" flag that decides whether the caustic frames are
        // reported to the live set at all.
        if (mGlowReader != nullptr)
            mGlowReader->beginFrame();

        const size_t texturesBeforeGlow = mTextures.size();

        const bool detached = refreshMovedObjects();

        // The glow's caustic frames are the one set of textures in this renderer first named on the
        // far side of the sync above, and they have to be: the glow is read out of
        // refreshMovedObjects, and refreshMovedObjects has to run after the cell adds. Without a
        // sync on this side of it a caustic sits in mTextures with no sampler slot, textureSlot
        // answers 0, GlowReader::read sees the white fallback and reports no glow -- and for a
        // player standing still in a room, forever. All 32 frames of textures/magicitem/caust*.dds
        // did exactly that.
        //
        // Gated on a texture having actually been loaded rather than run unconditionally, and the
        // gate is the important half. Two seconds of flipbook is 32 first sightings and then no
        // more, so this costs 32 syncs while an enchanted item is first in view and nothing at all
        // afterwards. Running it every frame instead makes collectLiveTextures walk every instance
        // of every loaded cell every frame, and -- far worse -- advances mTextureSyncCounter sixty
        // times a second, so the 300-tick residency grace stops meaning five seconds of standing
        // still and starts expiring the previous cell's whole texture set in one frame. Every one of
        // those expiries is a renumbering. See syncTexturesToRenderer for what a renumbering does to
        // the ray traced bounce; that is the measured room-brightening, not a cost.
        if (mTextures.size() != texturesBeforeGlow)
            syncTexturesToRenderer();


        // The TLAS is only rebuilt when the instance set actually changes, not every frame: a cell
        // load or unload, or the first time an object moves and therefore has to leave it. A swinging
        // door costs one rebuild at the moment it starts moving and none afterwards, because
        // MovedObject::detached is latched.
        if (changed || detached)
            mRenderer->markTlasDirty();
    }

    bool VkRenderingManager::refreshMovedObjects()
    {
        bool detachedAny = false;

        for (auto& [store, cellMeshes] : mCellMeshes)
        {
            for (CellMeshes::MovedObject& tracked : cellMeshes.tracked)
            {
                // The glow, read off this object every frame whether or not it has moved.
                //
                // It rides in this sweep rather than in a walk of its own because the list is
                // already here and already being iterated: the marginal cost of a glow read on a
                // non-glowing object is one node mask test and one stateset pointer, against the
                // Vec3/Quat compares this loop was doing anyway. A third osg::NodeVisitor over the
                // whole scene, which is what the particle and sky readers each cost, would be paid
                // every frame to discover every frame that nothing in the cell is enchanted.
                //
                // The cost of sharing the list is that an object trackableNode rejected cannot
                // glow either. Those are object paging's shared sentinel and references whose node
                // placement disagrees with their baked transform -- merged scenery, not the
                // individually placed items enchantments are put on -- so nothing enchanted is
                // expected to fall in that set. It is the first thing to check if a known glowing
                // item does not.
                if (mGlowReader != nullptr && tracked.node != nullptr)
                {
                    float glowColour[3] = { 0.0f, 0.0f, 0.0f };
                    uint32_t glowTexture = 0;
                    mGlowReader->read(*tracked.node, glowColour, glowTexture);

                    // Written every frame, including the frame a temporary spell-cast glow expires
                    // and the read comes back false. Left unwritten, an Open spell would light a
                    // door up for a second and a half and then for the rest of the cell's life.
                    for (uint32_t i = 0; i < tracked.instanceCount; ++i)
                    {
                        const uint32_t index = tracked.firstInstance + i;
                        if (index >= cellMeshes.instances.size())
                            break;
                        CellMeshes::Instance& instance = cellMeshes.instances[index];
                        instance.glowColour[0] = glowColour[0];
                        instance.glowColour[1] = glowColour[1];
                        instance.glowColour[2] = glowColour[2];
                        instance.glowTexture = glowTexture;
                    }
                }

                // The scrolled UV offset, read off the live object this frame.
                //
                // Rides in this sweep for the same reason the glow does: the list is already here and
                // already being iterated, and it sits above the early-out below because a lava pool
                // that never moves still has to flow. Gated on uvAnimated, decided at addCell, so an
                // object that does not scroll costs one bool test -- without that gate every rock in
                // the cell would pay for a subgraph walk to discover it has no TexMat.
                if (tracked.uvAnimated && tracked.node != nullptr)
                {
                    for (uint32_t i = 0; i < tracked.instanceCount; ++i)
                    {
                        const uint32_t index = tracked.firstInstance + i;
                        if (index >= cellMeshes.instances.size())
                            break;
                        CellMeshes::Instance& instance = cellMeshes.instances[index];
                        const std::string& name = mMeshes[instance.meshIndex]->uvControllerNode;
                        if (name.empty())
                            continue;
                        // Written every frame, including the frame the read fails: a stale offset
                        // would leave the surface stopped at wherever it happened to be.
                        instance.uvScroll[0] = 0.0f;
                        instance.uvScroll[1] = 0.0f;
                        readUvScroll(*tracked.node, name, sMaxUvDepth, instance.uvScroll);
                    }
                }

                const SceneUtil::PositionAttitudeTransform& node = *tracked.node;

                // This compare is the entire per-frame cost of the feature for the overwhelming
                // majority of objects, which are rocks and will never move. Ten floats against ten
                // floats, on memory OSG has usually just touched during the update traversal.
                //
                // Exact equality, not a tolerance. These are copies of the same floats OSG holds, so
                // anything nothing has written to is bit-identical -- and a tolerance would make the
                // slow end of a rising platform invisible, which is the case hardest to notice and
                // hardest to explain afterwards.
                //
                // All three, not just the position. A door swings: its position never changes and its
                // attitude is the whole motion, so testing position alone finds nothing at all and
                // would leave this fix doing nothing for the case it was written for.
                if (node.getPosition() == tracked.position && node.getAttitude() == tracked.attitude
                    && node.getScale() == tracked.scale)
                    continue;

                tracked.position = node.getPosition();
                tracked.attitude = node.getAttitude();
                tracked.scale = node.getScale();

                float objectTransform[16];
                makeNodeTransform(node, objectTransform);

                for (uint32_t i = 0; i < tracked.instanceCount; ++i)
                {
                    CellMeshes::Instance& instance
                        = cellMeshes.instances[tracked.firstInstance + i];
                    // The same product addCell formed, with a new left-hand side. The converter baked
                    // each submesh's place in the NIF hierarchy into mesh->transform and that part of
                    // the object does not move, so only the object matrix is new.
                    Vk::multiplyMat4(
                        objectTransform, mMeshPlacements[instance.meshIndex].data(), instance.transform);
                    instance.traced = false;
                }

                // Once per object, not once per frame of its motion. The rebuild it asks for idles the
                // device, so a door that announced itself on every frame of a one-second swing would
                // stall the device sixty times to open.
                if (!tracked.detached)
                {
                    tracked.detached = true;
                    detachedAny = true;
                }
            }
        }

        return detachedAny;
    }

    void VkRenderingManager::addActorInstances(const MWWorld::Ptr& ptr)
    {
        VFS::Path::Normalized model;
        try
        {
            model = ptr.getClass().getCorrectedModel(ptr);
        }
        catch (const std::exception&)
        {
            return;
        }

        if (model.empty())
            return;

        float objectTransform[16];
        makeObjectTransform(ptr, objectTransform);

        // An NPC's own model is meshes/base_anim.nif, a skeleton with no geometry in it, so this
        // returns nothing for them and the body has to be assembled from parts instead. Creatures
        // resolve to their own mesh and go straight through.
        const std::vector<size_t>* meshIndices = getOrLoadMeshes(std::string(model.value()));
        if (meshIndices == nullptr || meshIndices->empty())
        {
            if (ptr.getType() == ESM::NPC::sRecordId)
                addNpcInstances(ptr, objectTransform);
            return;
        }

        for (size_t meshIndex : *meshIndices)
        {
            ActorInstance instance;
            instance.meshIndex = meshIndex;
            Vk::multiplyMat4(objectTransform, mMeshes[meshIndex]->transform, instance.transform);
            mActorInstances.push_back(instance);
        }
    }

    const std::unordered_map<std::string, std::array<float, 16>>* VkRenderingManager::getOrLoadSkeleton(
        const std::string& model)
    {
        const auto cached = mSkeletonCache.find(model);
        if (cached != mSkeletonCache.end())
            return &cached->second;

        std::unordered_map<std::string, std::array<float, 16>> bones;
        try
        {
            const VFS::Path::Normalized path(model);
            const Nif::NIFFilePtr nif = MWBase::Environment::get().getResourceSystem()->getNifFileManager()->get(path);
            if (nif != nullptr)
                bones = NifVk::MeshConverter::collectNodeTransforms(Nif::FileView(*nif));
        }
        catch (const std::exception& e)
        {
            // Cached empty, like a failed mesh: a skeleton that cannot be read will not start working,
            // and every NPC of that race would otherwise retry it on every frame.
            Log(Debug::Warning) << "Vulkan: failed to load skeleton " << model << ": " << e.what();
        }

        return &mSkeletonCache.emplace(model, std::move(bones)).first->second;
    }

    void VkRenderingManager::addNpcInstances(const MWWorld::Ptr& ptr, const float objectTransform[16])
    {
        const ESM::NPC* npc = ptr.get<ESM::NPC>()->mBase;
        if (npc == nullptr)
            return;

        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const ESM::Race* race = store.get<ESM::Race>().search(npc->mRace);
        if (race == nullptr)
            return;

        const bool female = !npc->isMale();
        const bool beast = (race->mData.mFlags & ESM::Race::Beast) != 0;

        // The skeleton the parts hang on. Same choice MWClass::Npc::getCorrectedModel makes, and it
        // has to match it or the bone names will not be the ones the parts expect.
        const std::string skeletonModel = beast ? "meshes/base_animkna.nif" : "meshes/base_anim.nif";
        const std::unordered_map<std::string, std::array<float, 16>>* bones = getOrLoadSkeleton(skeletonModel);
        if (bones == nullptr || bones->empty())
            return;

        // The pose the animation system is actually holding this frame, if there is one. Preferred
        // over the skeleton file's bind pose for the obvious reason: with the bind pose every NPC in
        // the world stands in the same T-shape forever, and with this one they stand, walk and turn
        // their heads. Bone::mMatrixInSkeletonSpace is in the actor's own space, which is exactly
        // what objectTransform expects to be handed.
        //
        // It is a *rigid* animation: each part follows its dominant bone as one piece, so joints
        // pull apart under strong deformation. That is the remaining gap and it needs real skinning,
        // which is a per-vertex weighted sum -- see section 5.
        SceneUtil::Skeleton* liveSkeleton = nullptr;
        if (MWRender::Animation* animation = MWBase::Environment::get().getWorld()->getAnimation(ptr))
            liveSkeleton = animation->getSkeleton();

        // Live pose if the skeleton has that bone, bind pose otherwise. Both are in the actor's own
        // space, so the caller does not have to care which it got.
        const auto lookupBone = [&](const std::string& name, float out[16]) -> bool {
            if (liveSkeleton != nullptr)
            {
                if (SceneUtil::Bone* bone = liveSkeleton->getBone(name))
                {
                    osgMatrixToMat4(bone->mMatrixInSkeletonSpace, *reinterpret_cast<Vk::Mat4*>(out));
                    return true;
                }
            }

            const auto found = bones->find(name);
            if (found == bones->end())
                return false;
            std::copy(found->second.begin(), found->second.end(), out);
            return true;
        };

        // Which bone each kind of part hangs from. The same table MWRender::NpcAnimation keeps, copied
        // rather than shared because that class is welded to OSG and constructing one is a scene graph
        // operation. Only the parts a naked NPC has are here: clothing, armour and held weapons are
        // their own piece of work and would mean walking the inventory store.
        struct PartBone
        {
            ESM::PartReferenceType part;
            const char* bone;
        };
        static const PartBone sPartBones[] = {
            { ESM::PRT_Neck, "Neck" },
            { ESM::PRT_Cuirass, "Chest" },
            { ESM::PRT_Groin, "Groin" },
            { ESM::PRT_RHand, "Right Hand" },
            { ESM::PRT_LHand, "Left Hand" },
            { ESM::PRT_RWrist, "Right Wrist" },
            { ESM::PRT_LWrist, "Left Wrist" },
            { ESM::PRT_RForearm, "Right Forearm" },
            { ESM::PRT_LForearm, "Left Forearm" },
            { ESM::PRT_RUpperarm, "Right Upper Arm" },
            { ESM::PRT_LUpperarm, "Left Upper Arm" },
            { ESM::PRT_RFoot, "Right Foot" },
            { ESM::PRT_LFoot, "Left Foot" },
            { ESM::PRT_RAnkle, "Right Ankle" },
            { ESM::PRT_LAnkle, "Left Ankle" },
            { ESM::PRT_RKnee, "Right Knee" },
            { ESM::PRT_LKnee, "Left Knee" },
            { ESM::PRT_RLeg, "Right Upper Leg" },
            { ESM::PRT_LLeg, "Left Upper Leg" },
            { ESM::PRT_Tail, "Tail" },
        };

        const auto placeAt = [&](const std::string& partModel, const char* boneName) {
            if (partModel.empty())
                return;

            float boneMatrix[16];
            if (!lookupBone(boneName, boneMatrix))
                return;

            const std::vector<size_t>* partMeshes = getOrLoadMeshes(partModel);
            if (partMeshes == nullptr)
                return;

            float actorBone[16];
            Vk::multiplyMat4(objectTransform, boneMatrix, actorBone);

            for (size_t meshIndex : *partMeshes)
            {
                ActorInstance instance;
                instance.meshIndex = meshIndex;

                // Four cases, and getting them mixed up is what makes an NPC a heap of parts.
                //
                // A skinned part with a skin buffer is posed per vertex by the vertex shader. Its
                // vertices are in the skeleton's bind space, so its instance transform is the
                // actor's placement alone and the pose lives entirely in the bone palette.
                //
                // A skinned part with no buffer -- the palette was full, or the file named no bones
                // this converter could use -- falls back to its heaviest bone: actor * that bone's
                // world * that bone's inverse bind. Its node transform is not used, the skin
                // replaces it. Hanging a skinned part off the attachment bone as though it were
                // rigid applies a bone twice and throws it across the room; leaving the bone out
                // entirely drops it at the actor's feet. Both were tried and both look like a
                // broken NIF.
                //
                // A skinned part whose dominant bone is not in this skeleton either falls back
                // again, to the attachment bone, which is wrong but local.
                //
                // A rigid part is authored in the space of the bone that holds it: actor * that
                // bone * the part's own place in its file.

                const NifVk::VulkanMesh& mesh = *mMeshes[meshIndex];
                float skinBoneMatrix[16];
                if (mesh.skinBuffer && !mesh.skinBones.empty()
                    && mSkinMatrices.size() / 16 + mesh.skinBones.size() <= Vk::maxSkinMatrices)
                {
                    // Real skinning. The vertices are in the skeleton's bind space, so the instance
                    // transform is the actor's own placement and nothing else; the pose is entirely
                    // in the palette.
                    std::copy(objectTransform, objectTransform + 16, instance.transform);
                    instance.boneOffset = static_cast<uint32_t>(mSkinMatrices.size() / 16);

                    for (size_t bone = 0; bone < mesh.skinBones.size(); ++bone)
                    {
                        float boneWorld[16];
                        float palette[16];
                        if (lookupBone(mesh.skinBones[bone], boneWorld))
                        {
                            Vk::multiplyMat4(boneWorld, mesh.skinInvBinds[bone].data(), palette);
                        }
                        else
                        {
                            // Identity, not zero and not the inverse bind: in the bind pose a bone's
                            // world transform is exactly the inverse of its inverse bind, so identity
                            // is "leave these vertices where the file put them". A bone this skeleton
                            // does not have then costs its vertices nothing but the pose.
                            Vk::identityMat4(palette);
                        }
                        mSkinMatrices.insert(mSkinMatrices.end(), palette, palette + 16);

                        // The same bounds through the same matrix the vertices go through, unioned
                        // over the bones. Done here rather than at submission time because this is
                        // where the palette exists; recomputing it later would mean keeping it.
                        Vk::Mat4 posed;
                        Vk::multiplyMat4(objectTransform, palette, posed.data);

                        float boneMin[3];
                        float boneMax[3];
                        Vk::transformBounds(posed, mesh.boundsMin, mesh.boundsMax, boneMin, boneMax);

                        if (bone == 0)
                        {
                            std::copy(boneMin, boneMin + 3, instance.worldMin);
                            std::copy(boneMax, boneMax + 3, instance.worldMax);
                        }
                        else
                        {
                            for (int axis = 0; axis < 3; ++axis)
                            {
                                instance.worldMin[axis] = std::min(instance.worldMin[axis], boneMin[axis]);
                                instance.worldMax[axis] = std::max(instance.worldMax[axis], boneMax[axis]);
                            }
                        }
                    }
                }
                else if (mesh.skinned && !mesh.skinBone.empty() && lookupBone(mesh.skinBone, skinBoneMatrix))
                {
                    float actorSkinBone[16];
                    Vk::multiplyMat4(objectTransform, skinBoneMatrix, actorSkinBone);
                    Vk::multiplyMat4(actorSkinBone, mesh.skinInvBind, instance.transform);
                }
                else
                {
                    Vk::multiplyMat4(actorBone, mesh.transform, instance.transform);
                }

                mActorInstances.push_back(instance);
            }
        };

        // Which slots something is already covering. Clothing and armour go on first and the bare
        // body fills what is left, which is what upstream's priority system amounts to for a
        // standing NPC. Without it a shirt and a bare chest occupy the same space and z-fight.
        std::array<bool, ESM::PRT_Count> covered = {};

        const auto boneForPart = [&](int part) -> const char* {
            for (const PartBone& entry : sPartBones)
            {
                if (static_cast<int>(entry.part) == part)
                    return entry.bone;
            }
            return nullptr;
        };

        // Everything the NPC is wearing. Each clothing or armour record names a body part per slot
        // it covers, separately for male and female, and those parts hang off the same bones the
        // bare body does.
        const auto wearParts = [&](const ESM::PartReferenceList& list) {
            for (const ESM::PartReference& reference : list.mParts)
            {
                if (reference.mPart >= ESM::PRT_Count)
                    continue;

                // A record often names only one of the two. Falling back to the other is what
                // upstream does and is why a female NPC in a male-only shirt is dressed rather than
                // topless.
                const ESM::RefId& preferred = female ? reference.mFemale : reference.mMale;
                const ESM::RefId& fallback = female ? reference.mMale : reference.mFemale;
                const ESM::RefId& chosen = preferred.empty() ? fallback : preferred;
                if (chosen.empty())
                    continue;

                const ESM::BodyPart* part = store.get<ESM::BodyPart>().search(chosen);
                if (part == nullptr)
                    continue;

                const char* bone = boneForPart(reference.mPart);
                if (bone == nullptr)
                    continue;

                covered[reference.mPart] = true;
                placeAt(Misc::ResourceHelpers::correctMeshPath(part->mModel.getNormalized()).value(), bone);
            }
        };

        try
        {
            MWWorld::InventoryStore& inventory = ptr.getClass().getInventoryStore(ptr);
            for (int slot = 0; slot < MWWorld::InventoryStore::Slots; ++slot)
            {
                MWWorld::ContainerStoreIterator equipped = inventory.getSlot(slot);
                if (equipped == inventory.end())
                    continue;

                if (equipped->getType() == ESM::Clothing::sRecordId)
                    wearParts(equipped->get<ESM::Clothing>()->mBase->mParts);
                else if (equipped->getType() == ESM::Armor::sRecordId)
                    wearParts(equipped->get<ESM::Armor>()->mBase->mParts);
                else if (slot == MWWorld::InventoryStore::Slot_CarriedRight
                    || slot == MWWorld::InventoryStore::Slot_CarriedLeft)
                {
                    // A held weapon, torch or shield is not a body part: it is an ordinary object
                    // model hung on a bone of its own.
                    //
                    // Which bone the right hand uses depends on the weapon's type, and the mapping
                    // is data rather than convention: ESM::WeaponType::mAttachBone. A bow hangs off
                    // "Weapon Bone Left", a crossbow off "Weapon Bone", a thrown weapon off its own.
                    // Getting it wrong puts a bow through the NPC's chest rather than in its hand.
                    //
                    // The same fallback the OSG path uses, and for the same reason: a skeleton that
                    // does not have the named bone -- older or modded content -- gets "Weapon Bone",
                    // which every skeleton has. See MWRender::CreatureWeaponAnimation::updatePart.
                    // An equipped weapon is only in the hand while it is drawn. The OSG path hangs
                    // that off NpcAnimation::showWeapons, which the character controller drives from
                    // the draw state; read the draw state directly instead, since there is no
                    // animation object here to be told. Without this an NPC standing at a bar walks
                    // around holding a sword nobody drew, which is geometry the reference image does
                    // not have. What is *not* skipped is the sheathed weapon on the back: that needs
                    // "Bip01 AttachShield" and is a separate piece of work.
                    if (slot == MWWorld::InventoryStore::Slot_CarriedRight
                        && ptr.getClass().getCreatureStats(ptr).getDrawState() != MWMechanics::DrawState::Weapon)
                        continue;

                    std::string bone = "Shield Bone";
                    if (slot == MWWorld::InventoryStore::Slot_CarriedRight)
                    {
                        bone = "Weapon Bone";
                        if (equipped->getType() == ESM::Weapon::sRecordId)
                        {
                            const int weaponType = equipped->get<ESM::Weapon>()->mBase->mData.mType;
                            const std::string attachBone(MWMechanics::getWeaponType(weaponType)->mAttachBone);
                            float unused[16];
                            if (!attachBone.empty() && lookupBone(attachBone, unused))
                                bone = attachBone;
                        }
                    }

                    try
                    {
                        const std::string itemModel(equipped->getClass().getCorrectedModel(*equipped).value());
                        covered[ESM::PRT_Weapon] = true;
                        placeAt(itemModel, bone.c_str());
                    }
                    catch (const std::exception&)
                    {
                    }
                }
            }
        }
        catch (const std::exception&)
        {
            // Not every NPC has an inventory store, and one that throws should still get a body.
        }

        const std::vector<const ESM::BodyPart*>& parts
            = MWRender::NpcAnimation::getBodyParts(npc->mRace, female, false, false);

        for (const PartBone& entry : sPartBones)
        {
            if (static_cast<size_t>(entry.part) >= parts.size())
                continue;
            if (covered[entry.part])
                continue;
            const ESM::BodyPart* part = parts[entry.part];
            if (part == nullptr)
                continue;
            placeAt(Misc::ResourceHelpers::correctMeshPath(part->mModel.getNormalized()).value(), entry.bone);
        }

        // Head and hair are named on the NPC record itself rather than coming from the race's part
        // list, because they are the two things character creation lets the player choose.
        if (const ESM::BodyPart* head = store.get<ESM::BodyPart>().search(npc->mHead))
            placeAt(Misc::ResourceHelpers::correctMeshPath(head->mModel.getNormalized()).value(), "Head");
        if (const ESM::BodyPart* hair = store.get<ESM::BodyPart>().search(npc->mHair))
            placeAt(Misc::ResourceHelpers::correctMeshPath(hair->mModel.getNormalized()).value(), "Head");
    }

    void VkRenderingManager::syncActors(const std::set<MWWorld::CellStore*, std::less<>>& activeCells)
    {
        mActorInstances.clear();
        // Cleared together with the instances that index into it. An offset from last frame means
        // nothing this frame: the actors have moved and the palettes were rebuilt in whatever order
        // the cell walk produced.
        mSkinMatrices.clear();

        for (MWWorld::CellStore* cell : activeCells)
        {
            // forEach rather than forEachConst: an NPC's clothing and armour come out of their
            // inventory store, and MWWorld::Class::getInventoryStore takes a non-const Ptr.
            cell->forEach([&](const MWWorld::Ptr& ptr) {
                if (ptr.getClass().isActor())
                    addActorInstances(ptr);
                return true;
            });
        }

        // The player is in no cell's reference list -- MWWorld::Player holds their LiveCellRef as a
        // member and hands out a Ptr to it -- so the cell walk above can never find them, however
        // thorough it is.
        //
        // Except in first person, where their body is not drawn at all. OSG swaps the third-person
        // body for a separate hands-and-arms model there; this path has no equivalent yet, and
        // drawing the third-person one from inside its own chest is worse than drawing nothing --
        // an arm hangs into the bottom of the frame where the reference image has clear ground.
        const MWRender::Camera* camera = MWBase::Environment::get().getWorld()->getCamera();
        const bool firstPerson = camera != nullptr && camera->getMode() == MWRender::Camera::Mode::FirstPerson;

        const MWWorld::Ptr player = MWBase::Environment::get().getWorld()->getPlayerPtr();
        if (!player.isEmpty() && !firstPerson)
            addActorInstances(player);
    }

    void VkRenderingManager::resize(uint32_t width, uint32_t height)
    {
        mRenderer->resize(width, height);
    }

    void VkRenderingManager::requestScreenshot()
    {
        mRenderer->requestScreenshot();
    }

    std::filesystem::path VkRenderingManager::writeScreenshot(
        const std::filesystem::path& screenshotPath, const std::string& format)
    {
        std::vector<uint8_t> pixels;
        uint32_t width = 0;
        uint32_t height = 0;
        if (!mRenderer->takeScreenshot(pixels, width, height))
            return {};

        osg::ref_ptr<osg::Image> image = new osg::Image;
        image->allocateImage(static_cast<int>(width), static_cast<int>(height), 1, GL_RGBA, GL_UNSIGNED_BYTE);

        // Vulkan hands back the top row first and osg::Image is bottom row first, so the rows are
        // reversed on the way in. Getting this wrong produces a vertically mirrored screenshot, which
        // is easy to miss on a scene without text in it and then reads as a projection bug.
        const size_t rowBytes = static_cast<size_t>(width) * 4;
        for (uint32_t y = 0; y < height; ++y)
            std::memcpy(image->data(0, static_cast<int>(height - 1 - y)), pixels.data() + rowBytes * y, rowBytes);

        // A prefix of its own, so a comparison harness can tell the two backends' screenshots apart
        // by name instead of by the order they were written in.
        return SceneUtil::writeScreenshotToFile(screenshotPath, format, *image, "vulkan");
    }

    std::vector<bool> VkRenderingManager::collectLiveTextures()
    {
        std::vector<bool> live(mTextures.size(), false);
        mTextureLastUsed.resize(mTextures.size(), 0);
        ++mTextureSyncCounter;

        const auto mark = [&](size_t index) {
            if (index != sNoTexture && index < live.size())
            {
                live[index] = true;
                mTextureLastUsed[index] = mTextureSyncCounter;
            }
        };

        for (const auto& [store, cellMeshes] : mCellMeshes)
        {
            for (const auto& instance : cellMeshes.instances)
            {
                if (instance.meshIndex < mMeshTextures.size())
                    mark(mMeshTextures[instance.meshIndex]);
            }
        }

        // Actors are not in mCellMeshes and their meshes are usually referenced by nothing else, so
        // without this their textures look dead and get evicted -- and an evicted texture resolves
        // to the white fallback, which for alpha-tested geometry undoes trap 13 rather than merely
        // looking plain.
        for (const auto& instance : mActorInstances)
        {
            if (instance.meshIndex < mMeshTextures.size())
                mark(mMeshTextures[instance.meshIndex]);
        }

        // Terrain holds its texture index directly rather than going through a mesh.
        for (const auto& [store, cellTerrain] : mCellTerrain)
        {
            for (const auto& chunk : cellTerrain.chunks)
                mark(chunk.textureIndex);
        }

        // Particle textures belong to no mesh, no actor and no terrain chunk -- the geometry they are
        // drawn on does not exist until the frame is being built. Without this they are dead the
        // instant they are loaded, get replaced by the white fallback, and every flame, spark and
        // puff of smoke in the game draws as a solid white quad.
        if (mParticleReader != nullptr)
        {
            for (const size_t index : mParticleReader->textureIndices())
                mark(index);
        }

        // The sun and moon textures belong to no mesh either, and they are worse to lose: a moon that
        // has been evicted and replaced by the white fallback is a solid white disc in the night sky.
        if (mSkyReader != nullptr)
        {
            for (const size_t index : mSkyReader->textureIndices())
                mark(index);
        }

        // Every caustic frame of the enchanted glow, not the one bound this frame. A frame is up
        // for a sixteenth of a second and comes round again two seconds later, so reported one at a
        // time the other thirty-one look dead and lose their slots -- and the grace period below
        // does not save them, because two seconds is 120 frames and the grace is 60. An evicted
        // caustic resolves to the white fallback, which is not a dimmer glow but a solid
        // object-shaped block of the enchantment colour added over the scene.
        if (mGlowReader != nullptr)
        {
            for (const size_t index : mGlowReader->textureIndices())
                mark(index);
        }

        // The water normal map belongs to no cell either, and for a stronger reason than the
        // particle textures do: the surface it is drawn on is generated in water.vert and exists
        // nowhere in mCellTerrain. Without this it is dead the moment it loads, loses its slot, and
        // resolves to the 1x1 white fallback -- which decodes to a normal of (1, 1, 1), flattening
        // every wave in the game and tilting what is left the same way.
        mark(mWaterNormalTexture);

        // And anything used recently, not only this frame. See mTextureLastUsed: without this, every
        // frame of an animated texture except the one currently bound looks dead, loses its slot and
        // draws as the white fallback the moment the controller swings back to it.
        for (size_t i = 0; i < live.size(); ++i)
        {
            if (live[i] || mTextureLastUsed[i] == 0)
                continue;
            if (mTextureSyncCounter - mTextureLastUsed[i] <= sTextureSlotGraceFrames)
                live[i] = true;
        }

        return live;
    }

    void VkRenderingManager::syncTexturesToRenderer()
    {
        const std::vector<bool> live = collectLiveTextures();

        // Compaction and eviction are separate decisions and it matters that they are.
        //
        // Compacting the sampler array down to the live set is what fixes the overflow bug, and it is
        // free -- no device memory changes hands, only which slot each texture occupies. It therefore
        // happens every time.
        //
        // Actually *freeing* a texture only reclaims VRAM, and doing it the moment a cell unloads is
        // actively harmful: the cell grid churns constantly as the player walks, and a texture
        // dropped on one grid crossing is usually wanted again on the next. Measured during a plain
        // cell load that cost seven evictions and six immediate reloads, and every reload is a
        // blocking staging submit on the load path -- the exact cost this renderer already has too
        // much of. So eviction waits until enough textures are resident to be worth reclaiming, which
        // in practice means it never fires while the player stays in one region.
        const size_t resident = static_cast<size_t>(
            std::count_if(mTextures.begin(), mTextures.end(), [](const auto& t) { return t != nullptr; }));
        // The coupling vkrenderingmanager.hpp cannot express, because it does not include
        // vkrenderer.hpp. A texture resident past the end of the sampler array gets no slot and draws
        // as the white fallback -- which is what turned Ghostgate's Tower of Dusk into blank white
        // walls when this limit was 1024 against an array of 512.
        static_assert(sTextureResidencyLimit < Vk::maxSceneTextures,
            "texture residency must stay inside the sampler array, or the excess renders white");

        const bool evictNow = resident > sTextureResidencyLimit;

        // Reload anything live that was evicted earlier. This is not a rare corner: a cell that is
        // walked out of and back into hits mMeshCache on the way back, so getOrLoadTexture is never
        // called for it a second time and nothing else would ever notice its texture had gone. The
        // symptom would be a cell that renders correctly the first time and white afterwards.
        size_t reloaded = 0;
        for (size_t i = 0; i < mTextures.size(); ++i)
        {
            if (live[i] && mTextures[i] == nullptr && i < mTextureNames.size()
                && !mTextureNames[i].empty())
            {
                // The srgb flag has to survive the round trip. It cannot fire for the water normal
                // map today, because collectLiveTextures marks it live and only dead textures are
                // evicted -- but that is a coupling between two distant functions, and getting it
                // wrong reloads the normal map as sRGB and tilts every wave.
                getOrLoadTexture(mTextureNames[i], i != mWaterNormalTexture);
                if (mTextures[i] != nullptr)
                    ++reloaded;
            }
        }

        mTextureSlots.assign(mTextures.size(), 0);

        std::vector<VkImageView> views;
        views.reserve(mTextures.size());

        size_t evicted = 0;
        for (size_t i = 0; i < mTextures.size(); ++i)
        {
            if (!live[i])
            {
                // Not live, so it gets no slot either way -- that alone is what keeps the array
                // bounded. Freeing the memory is the optional part, and only happens once enough is
                // resident to be worth the reload it may cost.
                //
                // Keeping the index and the name is what lets every stored index stay valid across
                // an eviction. Safe here because Renderer::setTextures idles the device before
                // rewriting the descriptor array, so nothing is mid-flight against these views.
                if (evictNow && mTextures[i] != nullptr)
                {
                    mTextures[i].reset();
                    ++evicted;
                }
                continue;
            }

            if (mTextures[i] == nullptr)
                continue; // live but unloadable; falls back to white, as it did before eviction

            // Slot 0 is the white fallback, so live textures start at 1.
            views.push_back(mTextures[i]->view());
            mTextureSlots[i] = static_cast<uint32_t>(views.size());
        }

        // Compare the actual view list, not just its length. syncCells runs every frame and the live
        // set can change without changing size -- one cell's texture swapped for another's -- and
        // setTextures idles the device, so a length-only check would let a full pipeline stall
        // through on a frame where nothing needed rewriting.
        if (views == mUploadedTextureViews)
            return;

        // Whether the slot numbers already handed out still mean what they meant.
        //
        // Slot s reads views[s - 1], so as long as the old list survives as a prefix of the new one,
        // every number in circulation still names the texture it was issued for and all that has
        // happened is that fresh slots appeared at the end. Anything else -- a texture dropping out
        // of the live set, the list getting shorter -- packs the survivors down and shifts every
        // slot above the gap.
        const bool renumbered = mUploadedTextureViews.size() > views.size()
            || !std::equal(mUploadedTextureViews.begin(), mUploadedTextureViews.end(), views.begin());

        mRenderer->setTextures(views);
        mUploadedTextureViews = views;

        // The ray tracing side keeps its own copy of every slot and nothing in this function can
        // reach it. Renderer::buildTlas fills the geometry table from one frame's draw commands, no
        // other code ever rewrites it, and closesthit.rchit and anyhit.rahit index the sampler array
        // with what they find there. So a renumbering that is not followed by a rebuild leaves the
        // hit shaders on the previous numbering until something else happens to dirty the
        // acceleration structure -- which, for a player standing still in a room, is never.
        //
        // That is not a cosmetic error, and it is the failure this whole function was suspected of.
        // The closest-hit shader hands raygen the albedo of the bounce surface, and composite.frag
        // divides by (1 - that albedo) to stand in for the bounces it does not trace. A slot that has
        // shifted past the end of the now-shorter array reads the 1x1 white fallback: albedo 1,
        // clamped to 0.85, which multiplies the indirect term by 6.7 where Morrowind's dark interior
        // stone gives about 1.09. Meanwhile every texture in the image is still right, because the
        // raster path resolves its slots fresh in render() -- so the room comes out three and a half
        // times brighter than OSG's with nothing visibly untextured to explain it. Ghostgate's Tower
        // of Dusk measured 86.5 mean luma against 24.3 that way.
        //
        // The two numberings stay in step today only because syncCells happens to rebuild the
        // acceleration structure on the same condition it syncs textures on. That is a coincidence
        // of two adjacent ifs, and it breaks the moment either one is allowed to fire on its own.
        // This does not rely on it.
        if (renumbered)
            mRenderer->markTlasDirty();

        if (evicted > 0 || reloaded > 0)
        {
            Log(Debug::Info) << "Vulkan: " << views.size() << " textures live, " << evicted
                             << " evicted, " << reloaded << " reloaded, " << mTextures.size()
                             << " indices known";
        }
    }

    size_t VkRenderingManager::bakeLandComposite(const LandBlend& blend)
    {
        // One texture over the whole cell is not a blend, it is a texture. Drawing it through a
        // composite would cost a bake, a megabyte of VRAM and a resample of the diffuse for nothing.
        if (blend.layers.size() < 2 || blend.maps.size() != blend.layers.size())
            return sNoTexture;

        if (mLandComposite == nullptr)
            return sNoTexture;

        // The layers have to be resident before the bake reads them, and they go through the ordinary
        // texture path so they are cached, evictable and shared with anything else that uses them.
        std::vector<VkImageView> views;
        views.reserve(blend.layers.size());
        for (const std::string& layer : blend.layers)
        {
            const size_t index = getOrLoadTexture(layer);
            if (index == sNoTexture || mTextures[index] == nullptr)
                return sNoTexture;
            views.push_back(mTextures[index]->view());
        }

        auto baked = std::make_unique<Vk::Texture>(mLandComposite->bake(blend, views));
        if (!baked->valid())
            return sNoTexture;

        // Appended rather than cached by name, because a composite has no file name to key on and no
        // two cells share one. It still lives in mTextures, so eviction and the sampler array treat it
        // exactly like any other texture.
        const size_t index = mTextures.size();
        mTextures.push_back(std::move(baked));
        mTextureNames.emplace_back();
        return index;
    }

    size_t VkRenderingManager::getOrLoadTexture(const std::string& nifTextureName, bool srgb)
    {
        // A cached index whose texture is still resident, or a cached failure, is answered directly.
        // A cached index that was evicted falls through and reloads *into that same index*, which is
        // what lets mMeshTextures and the terrain chunks keep holding plain indices forever.
        size_t reloadInto = sNoTexture;
        const auto cached = mTextureCache.find(nifTextureName);
        if (cached != mTextureCache.end())
        {
            const size_t index = cached->second;
            if (index == sNoTexture || mTextures[index] != nullptr)
                return index;
            reloadInto = index;
        }

        size_t result = sNoTexture;
        try
        {
            Resource::ResourceSystem* resourceSystem = MWBase::Environment::get().getResourceSystem();
            Resource::ImageManager* imageManager = resourceSystem->getImageManager();

            // correctTexturePath handles Morrowind's conventions: the implicit "textures/" prefix and
            // .tga references that actually ship as .dds. Reused rather than reimplemented.
            const VFS::Path::Normalized corrected = Misc::ResourceHelpers::correctTexturePath(
                VFS::Path::Normalized(nifTextureName), *resourceSystem->getVFS());

            osg::ref_ptr<osg::Image> image = imageManager->getImage(corrected);
            if (image != nullptr && image->valid() && image->data() != nullptr)
            {
                const VkFormat format = toVkFormat(image->getPixelFormat(), srgb);
                const uint32_t width = static_cast<uint32_t>(image->s());
                const uint32_t height = static_cast<uint32_t>(image->t());
                // Morrowind's DDS files ship a full mip chain and osg keeps it in one contiguous
                // allocation, so upload all of it. Only level 0 used to be uploaded, which is why
                // distant terrain and foliage aliased so badly. getNumMipmapLevels() returns 1 for an
                // image without mips, which is the old behaviour exactly.
                const uint32_t levels = std::max(1u, static_cast<uint32_t>(image->getNumMipmapLevels()));
                const VkDeviceSize uploadSize
                    = Vk::Texture::mipChainSizeInBytes(format, width, height, levels);

                if (format != VK_FORMAT_UNDEFINED && uploadSize > 0)
                {
                    auto texture = std::make_unique<Vk::Texture>(
                        Vk::Texture::create(mRenderer->device(), mRenderer->commandPool(), width, height,
                            format, image->data(), uploadSize, levels));

                    if (reloadInto != sNoTexture)
                    {
                        mTextures[reloadInto] = std::move(texture);
                        result = reloadInto;
                    }
                    else
                    {
                        result = mTextures.size();
                        mTextures.push_back(std::move(texture));
                        mTextureNames.push_back(nifTextureName);
                    }
                }
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "Vulkan: failed to load texture " << nifTextureName << ": " << e.what();
            result = sNoTexture;
        }

        // A name that resolves to nothing draws as the 1x1 white fallback and says nothing about why.
        // The warning in resolveByName only covers the readers -- particles, sky, water, glow -- and
        // this is the path every mesh in the world goes down, which is the one where a silent failure
        // turns a room white. Warned once per name, on the load rather than on a repeat count, because
        // unlike a sampler slot a load failure here is cached and never retried.
        if (result == sNoTexture)
        {
            static std::set<std::string, Misc::StringUtils::CiComp> warnedMesh;
            if (warnedMesh.insert(nifTextureName).second)
                Log(Debug::Warning) << "Vulkan: mesh texture '" << nifTextureName
                                    << "' did not load; every mesh using it draws white";
        }

        // Cache failures too, so an unloadable texture is not retried for every mesh that uses it.
        // emplace deliberately, not insert_or_assign: on a reload the entry already exists and
        // already holds the right index.
        mTextureCache.emplace(nifTextureName, result);
        return result;
    }

    const std::vector<size_t>* VkRenderingManager::getOrLoadMeshes(const std::string& model)
    {
        const auto cached = mMeshCache.find(model);
        if (cached != mMeshCache.end())
            return &cached->second;

        std::vector<size_t> indices;
        try
        {
            const VFS::Path::Normalized path(model);
            const Nif::NIFFilePtr nif
                = MWBase::Environment::get().getResourceSystem()->getNifFileManager()->get(path);

            if (nif != nullptr)
            {
                std::vector<NifVk::VulkanMesh> converted = mMeshConverter->convert(Nif::FileView(*nif));
                indices.reserve(converted.size());
                for (NifVk::VulkanMesh& mesh : converted)
                {
                    const size_t textureIndex
                        = mesh.baseTexture.empty() ? sNoTexture : getOrLoadTexture(mesh.baseTexture);

                    // Where this submesh sits relative to the object's origin. Normally the shape's
                    // place in the NIF node hierarchy, which the converter has already baked into
                    // mesh.transform.
                    //
                    // A skinned shape is the exception: its vertices are in the skin's bind space,
                    // not the node's, and the node transform says nothing about where they belong.
                    // Every banner and flag in the game is skinned, and their bind space is a flat
                    // quad lying in the local XY plane -- 80 by 128 units with z exactly 0 at every
                    // vertex. The bone chain is what turns it upright and hangs it down; the inverse
                    // bind of every bone in furn_banner_hlaalu_01 is a 90 degree rotation about X.
                    // Place one of these by its node transform and it is drawn horizontal, which
                    // from a camera below it is a sliver a pixel or two tall.
                    //
                    // Rigid, one bone for the whole shape -- the heaviest, the same fallback
                    // syncActors uses for an actor part that could not get a palette. Flat instead
                    // of curved, and it will not blow in the wind because nothing here reads the
                    // NiKeyframeControllers, but both beat a banner nobody can see.
                    std::array<float, 16> placement;
                    std::memcpy(placement.data(), mesh.transform, sizeof(placement));
                    if (mesh.skinned && !mesh.skinBone.empty())
                    {
                        // The object's own file. A banner carries its Root Bone/Bone02/... chain
                        // alongside its geometry, so there is no separate skeleton to go and find.
                        // An actor part names bones that live in base_anim.nif instead, so this
                        // lookup misses and the node transform stands -- which is what keeps this
                        // away from actors, who are posed by syncActors and never read this vector.
                        if (const auto* restBones = getOrLoadSkeleton(model))
                        {
                            const auto bone = restBones->find(mesh.skinBone);
                            if (bone != restBones->end())
                                Vk::multiplyMat4(bone->second.data(), mesh.skinInvBind, placement.data());
                        }
                    }

                    indices.push_back(mMeshes.size());
                    mMeshes.push_back(std::make_unique<NifVk::VulkanMesh>(std::move(mesh)));
                    mMeshTextures.push_back(textureIndex);
                    mMeshPlacements.push_back(placement);
                }
            }
        }
        catch (const std::exception& e)
        {
            // Cache the failure as an empty list so a broken or unsupported model is not retried for
            // every reference that uses it.
            Log(Debug::Warning) << "Vulkan: failed to load mesh " << model << ": " << e.what();
            indices.clear();
        }

        return &mMeshCache.emplace(model, std::move(indices)).first->second;
    }
}

#endif // OPENMW_USE_VULKAN
