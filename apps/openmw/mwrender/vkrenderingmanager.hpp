#ifndef OPENMW_MWRENDER_VKRENDERINGMANAGER_H
#define OPENMW_MWRENDER_VKRENDERINGMANAGER_H

#ifdef OPENMW_USE_VULKAN

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct SDL_Window;

namespace Vk
{
    class Renderer;
    struct Mat4;
    struct SceneData;
}

namespace NifVk
{
    struct VulkanMesh;
    class MeshConverter;
}

namespace MWWorld
{
    class CellStore;
}

namespace MWRender
{
    class Camera;

    class VkRenderingManager
    {
    public:
        VkRenderingManager(SDL_Window* window, bool enableValidation);
        ~VkRenderingManager();

        void render(Camera& camera, float sunAzimuth, float sunAltitude);
        bool loadShaders(const std::filesystem::path& shaderDir);

        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);

    private:
        struct CellMeshes
        {
            struct Instance
            {
                size_t meshIndex;
                float transform[16];
            };
            std::vector<Instance> instances;
        };

        size_t getOrLoadMesh(const std::string& model);

        SDL_Window* mWindow;
        std::unique_ptr<Vk::Renderer> mRenderer;
        std::unique_ptr<NifVk::MeshConverter> mMeshConverter;

        int mDrawableWidth = 0;
        int mDrawableHeight = 0;

        std::unordered_map<std::string, size_t> mMeshCache;
        std::vector<std::unique_ptr<NifVk::VulkanMesh>> mMeshes;

        std::unordered_map<const MWWorld::CellStore*, CellMeshes> mCellMeshes;
    };
}

#endif // OPENMW_USE_VULKAN
#endif
