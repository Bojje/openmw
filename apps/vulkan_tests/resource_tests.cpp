#include <stdexcept>

#include <components/resource/resourcesystem.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>

int main()
{
    const VFS::Manager vfsManager;
    const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
    Resource::ResourceSystem resourceSystem(
        &vfsManager, 1.0, &encoder.getStatelessEncoder(), Resource::ResourceSystem::Backend::Neutral);

    if (resourceSystem.getSceneManager() != nullptr || resourceSystem.getKeyframeManager() != nullptr)
        throw std::runtime_error("neutral resource backend constructed OSG scene services");
    if (resourceSystem.getImageManager() == nullptr || resourceSystem.getNifMeshManager() == nullptr)
        throw std::runtime_error("neutral resource backend omitted shared resource services");
}
