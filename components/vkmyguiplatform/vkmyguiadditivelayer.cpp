#include "vkmyguiadditivelayer.hpp"

#include "vkmyguirendermanager.hpp"

namespace VkMyGUIPlatform
{

    void AdditiveLayer::renderToTarget(MyGUI::IRenderTarget* target, bool update)
    {
        RenderManager& renderManager = static_cast<RenderManager&>(MyGUI::RenderManager::getInstance());

        renderManager.setBlendMode(BlendMode::Additive);

        MyGUI::OverlappedLayer::renderToTarget(target, update);

        renderManager.setBlendMode(BlendMode::Alpha);
    }

}
