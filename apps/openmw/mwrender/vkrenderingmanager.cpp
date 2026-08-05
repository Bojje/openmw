#ifdef OPENMW_USE_VULKAN

#include "vkrenderingmanager.hpp"

#include <cmath>
#include <cstring>

#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>
#include <components/nifvk/meshconverter.hpp>
#include <components/vk/vkrenderer.hpp>

#include "../mwworld/cellstore.hpp"
#include "camera.hpp"

namespace MWRender
{
    VkRenderingManager::VkRenderingManager(SDL_Window* window, bool enableValidation)
        : mWindow(window)
    {
        mRenderer = std::make_unique<Vk::Renderer>(window, enableValidation);
        SDL_Vulkan_GetDrawableSize(mWindow, &mDrawableWidth, &mDrawableHeight);
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

    void VkRenderingManager::render(Camera& camera, float sunAzimuth, float sunAltitude)
    {
        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(mWindow, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
            return;

        if (drawableWidth != mDrawableWidth || drawableHeight != mDrawableHeight)
        {
            mRenderer->resize(static_cast<uint32_t>(drawableWidth), static_cast<uint32_t>(drawableHeight));
            mDrawableWidth = drawableWidth;
            mDrawableHeight = drawableHeight;
        }

        Vk::SceneData scene = {};

        const auto& viewMatrix = camera.getViewMatrix();
        const auto& projMatrix = camera.getProjectionMatrix();

        osgMatrixToMat4(viewMatrix, scene.view);
        osgMatrixToMat4(projMatrix, scene.projection);
        scene.viewInverse = Vk::invertMat4(scene.view);
        scene.projInverse = Vk::invertMat4(scene.projection);

        float cosAlt = std::cos(sunAltitude);
        scene.sunDirection = { std::sin(sunAzimuth) * cosAlt, std::cos(sunAzimuth) * cosAlt, std::sin(sunAltitude), 0.0f };
        scene.sunColor = { 1.0f, 0.95f, 0.85f, 1.0f };

        mRenderer->updateScene(scene);

        for (const auto& [store, cellMeshes] : mCellMeshes)
        {
            for (const auto& inst : cellMeshes.instances)
            {
                const auto& mesh = mMeshes[inst.meshIndex];
                Vk::Mat4 transform;
                std::memcpy(transform.data, inst.transform, sizeof(float) * 16);
                mRenderer->submitMesh(mesh->vertexBuffer->handle(), mesh->indexBuffer->handle(),
                    mesh->indexCount, transform);
            }
        }

        mRenderer->render();
    }

    void VkRenderingManager::addCell(const MWWorld::CellStore* /*store*/)
    {
        // Cell mesh loading will be implemented when MeshConverter is wired in.
        // For each object in the cell, we would:
        //   1. Get the model path from the object's class
        //   2. Load or retrieve the cached VulkanMesh
        //   3. Compute the world transform from the object's position/rotation
        //   4. Add an Instance to mCellMeshes
    }

    void VkRenderingManager::removeCell(const MWWorld::CellStore* store)
    {
        mCellMeshes.erase(store);
    }

    size_t VkRenderingManager::getOrLoadMesh(const std::string& /*model*/)
    {
        // Mesh loading via NifVk::MeshConverter will be connected here.
        // 1. Check mMeshCache for the model path
        // 2. If not found, load the NIF file and convert via mMeshConverter
        // 3. Cache and return the index
        return 0;
    }
}

#endif // OPENMW_USE_VULKAN
