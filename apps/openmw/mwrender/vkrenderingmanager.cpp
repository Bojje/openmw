#ifdef OPENMW_USE_VULKAN

#include "vkrenderingmanager.hpp"

#include <algorithm>
#include <map>
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

#include <components/sceneutil/skeleton.hpp>

#include "animation.hpp"
#include "npcanimation.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "camera.hpp"
#include "vklandcomposite.hpp"
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
            if (slot == 0)
            {
                static std::map<std::string, int, Misc::StringUtils::CiComp> failures;
                const int count = ++failures[std::string(stripped)];
                if (count == 30)
                    Log(Debug::Warning) << "Vulkan: texture '" << stripped
                                        << "' has failed to load " << count
                                        << " times; it is drawing as a white square";
            }

            return slot;
        };
        mParticleReader = std::make_unique<ParticleReader>(resolveByName);
        mSkyReader = std::make_unique<SkyReader>(resolveByName);
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
        mRenderer->setWaterNormalMap(waterNormalSlot);

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
        if (mSkyReader != nullptr)
            mRenderer->updateSky(mSkyReader->elements());

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
                submission.blasAddress
                    = mesh->blas ? mesh->blas->deviceAddress() : VkDeviceAddress{ 0 };
                submission.vertexAddress = mesh->vertexBuffer->deviceAddress();
                submission.indexAddress = mesh->indexBuffer->deviceAddress();
                // The sampler slot, not the storage index. Since eviction landed the two are
                // different: mTextureSlots holds 0 -- the white fallback -- for any texture no loaded
                // cell references, and a compacted slot for the ones they do.
                submission.textureIndex = textureSlot(textureIndex);
                submission.alphaTested = mesh->alphaTested;
                submission.roughness = mesh->roughness;
                submission.specularStrength = mesh->specularStrength;
                submission.visible = visible;

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

        mRenderer->render();
    }

    void VkRenderingManager::addCell(const MWWorld::CellStore* store)
    {
        if (store == nullptr || mCellMeshes.find(store) != mCellMeshes.end())
            return;

        CellMeshes cellMeshes;
        size_t skipped = 0;

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

            for (size_t meshIndex : *meshIndices)
            {
                CellMeshes::Instance instance;
                instance.meshIndex = meshIndex;
                // The converter bakes each submesh's place in the NIF node hierarchy into its own
                // transform, so the final instance transform is object * nifLocal.
                Vk::multiplyMat4(objectTransform, mMeshes[meshIndex]->transform, instance.transform);
                cellMeshes.instances.push_back(instance);
            }

            return true;
        });

        size_t textured = 0;
        for (const auto& mesh : mMeshes)
        {
            if (!mesh->baseTexture.empty())
                ++textured;
        }

        Log(Debug::Info) << "Vulkan: loaded cell with " << cellMeshes.instances.size()
                         << " instances (" << skipped << " models skipped), "
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

        // The TLAS is only rebuilt when the instance set actually changes, not every frame.
        if (changed)
            mRenderer->markTlasDirty();
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

    std::vector<bool> VkRenderingManager::collectLiveTextures() const
    {
        std::vector<bool> live(mTextures.size(), false);

        const auto mark = [&](size_t index) {
            if (index != sNoTexture && index < live.size())
                live[index] = true;
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

        // The water normal map belongs to no cell either, and for a stronger reason than the
        // particle textures do: the surface it is drawn on is generated in water.vert and exists
        // nowhere in mCellTerrain. Without this it is dead the moment it loads, loses its slot, and
        // resolves to the 1x1 white fallback -- which decodes to a normal of (1, 1, 1), flattening
        // every wave in the game and tilting what is left the same way.
        mark(mWaterNormalTexture);

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

        mRenderer->setTextures(views);
        mUploadedTextureViews = views;

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

                    indices.push_back(mMeshes.size());
                    mMeshes.push_back(std::make_unique<NifVk::VulkanMesh>(std::move(mesh)));
                    mMeshTextures.push_back(textureIndex);
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
