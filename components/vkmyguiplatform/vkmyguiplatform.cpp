#include "vkmyguiplatform.hpp"

#include <MyGUI_LogManager.h>

#include <components/myguiplatform/myguidatamanager.hpp>
#include <components/myguiplatform/myguiloglistener.hpp>

#include "vkmyguirendermanager.hpp"

namespace VkMyGUIPlatform
{

    Platform::Platform(Vk::Renderer& renderer, Resource::ImageManager* imageManager, const VFS::Manager* vfs,
        float uiScalingFactor, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logName)
        : mLogFacility(logName.empty() ? nullptr : std::make_unique<MyGUIPlatform::LogFacility>(logName, false))
        , mLogManager(std::make_unique<MyGUI::LogManager>())
        , mDataManager(std::make_unique<MyGUIPlatform::DataManager>(resourcePath, vfs))
        , mRenderManager(std::make_unique<RenderManager>(renderer, imageManager, uiScalingFactor))
    {
        if (mLogFacility != nullptr)
            mLogManager->addLogSource(mLogFacility->getSource());

        mRenderManager->initialise();
    }

    Platform::~Platform() = default;

    bool Platform::loadShaders(const std::string& shaderDir)
    {
        return mRenderManager->loadShaders(shaderDir);
    }

    void Platform::shutdown()
    {
        mRenderManager->shutdown();
    }

    RenderManager* Platform::getRenderManagerPtr()
    {
        return mRenderManager.get();
    }

    MyGUIPlatform::DataManager* Platform::getDataManagerPtr()
    {
        return mDataManager.get();
    }

}
