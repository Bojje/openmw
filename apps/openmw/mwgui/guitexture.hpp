#ifndef MWGUI_GUITEXTURE_H
#define MWGUI_GUITEXTURE_H

#include <memory>
#include <string>
#include <string_view>

namespace MyGUI
{
    class ITexture;
}

namespace osg
{
    class Image;
    class StateSet;
    class Texture2D;
}

namespace MWGui
{
    /// True when the interface is being drawn by the Vulkan platform rather than the OSG one.
    ///
    /// Asked of the live MyGUI render manager rather than of a setting, because that is the thing
    /// that actually decides: the platform is a session-long singleton and the setting only chose
    /// which one was built.
    bool usingVulkanGuiPlatform();

    /// Wraps an OSG texture for whichever MyGUI platform is live, so the eleven interface surfaces
    /// that come from OSG render-to-texture do not have to know which backend is drawing them.
    ///
    /// Under the OSG platform the GPU texture is used directly, as it always was. Under the Vulkan
    /// platform there is no shared texture and no interop -- deliberately, see HANDOFF section 1 --
    /// so the CPU image the texture carries is uploaded into a Vulkan texture instead. That works
    /// only for producers that keep an osg::Image alive; \a texture with no image returns nullptr,
    /// which leaves the caller with the blank surface it already had rather than something worse.
    ///
    /// \a name has to be unique per surface. MyGUI never looks these up by name -- the caller owns
    /// the returned texture -- but the name is what appears in a Vulkan validation message, so a
    /// meaningful one is worth the trouble.
    std::unique_ptr<MyGUI::ITexture> createGuiTexture(osg::Texture2D* texture, std::string_view name);

    /// The same, for a surface whose CPU copy does not live on the texture -- a render-to-texture
    /// read back separately, say. \a texture is used under the OSG platform and \a image under the
    /// Vulkan one, and neither backend is asked to know about the other's source.
    ///
    /// Returns nullptr when the Vulkan platform is live and \a image has not been filled yet, which
    /// for a readback means the render has not landed. Ask again next frame rather than treating it
    /// as a failure.
    ///
    /// \a stateSet is the state the OSG platform needs to draw the texture correctly -- the
    /// character previews hand it a premultiplied-alpha blend function this way. It applies to the
    /// OSG path only; the Vulkan platform has its own fixed pipeline state and ignores it.
    std::unique_ptr<MyGUI::ITexture> createGuiTexture(
        osg::Texture2D* texture, osg::Image* image, std::string_view name, osg::StateSet* stateSet = nullptr);
}

#endif
