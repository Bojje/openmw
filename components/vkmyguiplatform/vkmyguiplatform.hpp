#ifndef OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIPLATFORM_H
#define OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIPLATFORM_H

#include <filesystem>
#include <memory>
#include <string>

#include <components/vfs/pathutil.hpp>

namespace Resource
{
    class ImageManager;
}
namespace MyGUI
{
    class LogManager;
}
namespace VFS
{
    class Manager;
}
namespace Vk
{
    class Renderer;
}
namespace MyGUIPlatform
{
    class DataManager;
    class LogFacility;
}

namespace VkMyGUIPlatform
{

    class RenderManager;

    /// Mirrors MyGUIPlatform::Platform, and deliberately owns fewer things than it.
    ///
    /// Only the render manager is backend-specific. The data manager reads layouts, skins and fonts
    /// out of the VFS and the log facility writes MyGUI.log; neither has any idea a renderer exists,
    /// so both are used from components/myguiplatform rather than copied. That is not only tidier --
    /// components/fontloader reaches for MyGUIPlatform::DataManager by name through a dynamic_cast on
    /// the singleton, so a duplicate class here would silently break font loading.
    class Platform
    {
    public:
        Platform(Vk::Renderer& renderer, Resource::ImageManager* imageManager, const VFS::Manager* vfs,
            float uiScalingFactor, VFS::Path::NormalizedView resourcePath,
            const std::filesystem::path& logName = "MyGUI.log");

        ~Platform();

        /// Compiles the interface pipelines out of \a shaderDir, which is the same directory
        /// Vk::Renderer::loadShadersAndCreatePipelines is given.
        bool loadShaders(const std::string& shaderDir);

        void shutdown();

        RenderManager* getRenderManagerPtr();

        MyGUIPlatform::DataManager* getDataManagerPtr();

    private:
        std::unique_ptr<MyGUIPlatform::LogFacility> mLogFacility;
        std::unique_ptr<MyGUI::LogManager> mLogManager;
        std::unique_ptr<MyGUIPlatform::DataManager> mDataManager;
        std::unique_ptr<RenderManager> mRenderManager;
    };

}

#endif
