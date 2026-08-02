#ifdef OPENMW_USE_VULKAN

#include "vkrenderingmanager.hpp"

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
    constexpr size_t sNoTexture = static_cast<size_t>(-1);

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
        // Morrowind's colours -- cell mood, light diffuse, weather sun -- were authored by artists
        // looking at gamma-space compositing, and the OSG renderer still lights in gamma space. This
        // renderer lights in linear, so they have to be decoded on the way in or every one of them is
        // systematically too bright. Scalars such as sun visibility must NOT go through this.
        float srgbToLinear(float c)
        {
            return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }

        Vk::Vec4 decodeColor(const osg::Vec4f& c)
        {
            return { srgbToLinear(c.r()), srgbToLinear(c.g()), srgbToLinear(c.b()), c.a() };
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

        mRenderer->updateScene(scene);

        for (const auto& [store, cellMeshes] : mCellMeshes)
        {
            for (const auto& inst : cellMeshes.instances)
            {
                const auto& mesh = mMeshes[inst.meshIndex];
                Vk::Mat4 transform;
                std::memcpy(transform.data, inst.transform, sizeof(float) * 16);

                // Slot 0 of the renderer's sampler array is the white fallback and mTextures[i] sits
                // in slot i + 1, so the sNoTexture sentinel maps straight to 0. The renderer clamps
                // anything that does not fit in the array back to 0 as well.
                const size_t textureIndex = mMeshTextures[inst.meshIndex];

                Vk::MeshSubmission submission;
                submission.vertexBuffer = mesh->vertexBuffer->handle();
                submission.indexBuffer = mesh->indexBuffer->handle();
                submission.indexCount = mesh->indexCount;
                submission.transform = transform;
                submission.blasAddress
                    = mesh->blas ? mesh->blas->deviceAddress() : VkDeviceAddress{ 0 };
                submission.vertexAddress = mesh->vertexBuffer->deviceAddress();
                submission.indexAddress = mesh->indexBuffer->deviceAddress();
                submission.textureIndex
                    = textureIndex == sNoTexture ? 0u : static_cast<uint32_t>(textureIndex + 1);
                submission.alphaTested = mesh->alphaTested;
                submission.roughness = mesh->roughness;
                submission.specularStrength = mesh->specularStrength;

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
                submission.textureIndex = chunk.textureIndex == sNoTexture
                    ? 0u
                    : static_cast<uint32_t>(chunk.textureIndex + 1);
                // Terrain is a solid heightfield; leaving it opaque keeps the fast traversal path.
                submission.alphaTested = false;
                // Dirt and rock. No specular, which is also what the OSG renderer gives terrain.
                submission.roughness = 1.0f;
                submission.specularStrength = 0.0f;

                mRenderer->submitMesh(submission);
            }
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

        // Loading the cell may have uploaded new textures; the G-buffer pass cannot sample them until
        // the descriptor array is rewritten.
        syncTexturesToRenderer();

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
                ++it;
            else
                it = mCellTerrain.erase(it);
        }

        for (MWWorld::CellStore* cell : activeCells)
        {
            if (mCellMeshes.find(cell) != mCellMeshes.end())
                continue;
            addCell(cell);
            changed = true;
        }

        // The TLAS is only rebuilt when the instance set actually changes, not every frame.
        if (changed)
            mRenderer->markTlasDirty();
    }

    void VkRenderingManager::resize(uint32_t width, uint32_t height)
    {
        mRenderer->resize(width, height);
    }

    void VkRenderingManager::syncTexturesToRenderer()
    {
        if (mTextures.size() == mTexturesUploaded)
            return;

        std::vector<VkImageView> views;
        views.reserve(mTextures.size());
        for (const auto& texture : mTextures)
            views.push_back(texture ? texture->view() : VK_NULL_HANDLE);

        mRenderer->setTextures(views);
        mTexturesUploaded = mTextures.size();
    }

    size_t VkRenderingManager::getOrLoadTexture(const std::string& nifTextureName)
    {
        const auto cached = mTextureCache.find(nifTextureName);
        if (cached != mTextureCache.end())
            return cached->second;

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
                const VkDeviceSize uploadSize = levelZeroSizeInBytes(format, width, height);

                // Never read more than osg actually allocated, whatever the block maths says.
                const VkDeviceSize available = static_cast<VkDeviceSize>(image->getTotalSizeInBytes());

                if (format != VK_FORMAT_UNDEFINED && uploadSize > 0 && uploadSize <= available)
                {
                    auto texture = std::make_unique<Vk::Texture>(
                        Vk::Texture::create(mRenderer->device(), mRenderer->commandPool(), width, height,
                            format, image->data(), uploadSize));

                    result = mTextures.size();
                    mTextures.push_back(std::move(texture));
                }
            }
        }
        catch (const std::exception& e)
        {
            Log(Debug::Warning) << "Vulkan: failed to load texture " << nifTextureName << ": " << e.what();
            result = sNoTexture;
        }

        // Cache failures too, so an unloadable texture is not retried for every mesh that uses it.
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
