#ifdef OPENMW_USE_VULKAN

#include "vkrenderingmanager.hpp"

#include <algorithm>
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
#include <components/nif/niffile.hpp>
#include <components/nifvk/meshconverter.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vk/vkbuffer.hpp>
#include <components/vk/vkmath.hpp>
#include <components/vk/vkrenderer.hpp>
#include <components/vk/vktexture.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "camera.hpp"
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
    VkFormat toVkFormat(unsigned int glPixelFormat)
    {
        switch (glPixelFormat)
        {
            case sGlDxt1Rgb: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
            case sGlDxt1Rgba: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
            case sGlDxt3: return VK_FORMAT_BC2_SRGB_BLOCK;
            case sGlDxt5: return VK_FORMAT_BC3_SRGB_BLOCK;
            case sGlRgba: return VK_FORMAT_R8G8B8A8_SRGB;
            case sGlBgra: return VK_FORMAT_B8G8R8A8_SRGB;
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
        Log(Debug::Info) << "Vulkan renderer initialized";
    }

    VkRenderingManager::~VkRenderingManager() = default;

    bool VkRenderingManager::loadShaders(const std::filesystem::path& shaderDir)
    {
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
                             << " object instances";
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
        if (cell == nullptr || !cell->isExterior())
            return;

        const int cellX = cell->getGridX();
        const int cellY = cell->getGridY();

        std::vector<LandChunk> landChunks = buildLandChunks(cellX, cellY);
        if (landChunks.empty())
            return;

        CellTerrain terrain;

        // Vertices come out cell-local, so all that is left is the translation to the cell's
        // south-west corner. Column-vector convention, matching makeObjectTransform.
        Vk::identityMat4(terrain.transform);
        terrain.transform[12] = static_cast<float>(cellX) * ESM::Land::REAL_SIZE;
        terrain.transform[13] = static_cast<float>(cellY) * ESM::Land::REAL_SIZE;

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
            uploaded.textureIndex = getOrLoadTexture(chunk.texture);
            terrain.chunks.push_back(std::move(uploaded));

            triangles += indexCount / 3;
        }

        if (terrain.chunks.empty())
            return;

        syncTexturesToRenderer();

        Log(Debug::Info) << "Vulkan: loaded terrain for cell " << cellX << ", " << cellY << " -- "
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

    void VkRenderingManager::resize(uint32_t width, uint32_t height)
    {
        mRenderer->resize(width, height);
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

        // Terrain holds its texture index directly rather than going through a mesh.
        for (const auto& [store, cellTerrain] : mCellTerrain)
        {
            for (const auto& chunk : cellTerrain.chunks)
                mark(chunk.textureIndex);
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
                getOrLoadTexture(mTextureNames[i]); // reloads into index i, see the reloadInto path
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

    size_t VkRenderingManager::getOrLoadTexture(const std::string& nifTextureName)
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
                const VkFormat format = toVkFormat(image->getPixelFormat());
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
