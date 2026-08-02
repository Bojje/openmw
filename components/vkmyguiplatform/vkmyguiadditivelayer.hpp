#ifndef OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIADDITIVELAYER_H
#define OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIADDITIVELAYER_H

#include <MyGUI_OverlappedLayer.h>

namespace VkMyGUIPlatform
{

    /// A layer whose contents blend additively.
    ///
    /// The Vulkan counterpart of MyGUIPlatform::AdditiveLayer, and it has to be a separate class
    /// rather than a shared one because the mechanism differs: the OSG version injects an
    /// osg::StateSet carrying a BlendFunc, and there is nothing to inject into a recorded command
    /// buffer. Selecting between two pre-built pipelines is the same idea expressed in the terms
    /// Vulkan has.
    ///
    /// The RTTI name is deliberately "AdditiveLayer", matching the OSG class, because that is the
    /// string OpenMW's layout XML names. Only one platform is registered at a time, so the two never
    /// collide.
    class AdditiveLayer final : public MyGUI::OverlappedLayer
    {
    public:
        MYGUI_RTTI_DERIVED(AdditiveLayer)

        void renderToTarget(MyGUI::IRenderTarget* target, bool update) override;
    };

}

#endif
