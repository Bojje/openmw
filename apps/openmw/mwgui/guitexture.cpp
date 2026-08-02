#include "guitexture.hpp"

#include <cstring>

#include <MyGUI_RenderManager.h>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguitexture.hpp>

#ifdef OPENMW_USE_VULKAN
#include <components/vkmyguiplatform/vkmyguirendermanager.hpp>
#include <components/vkmyguiplatform/vkmyguitexture.hpp>
#endif

namespace
{
#ifdef OPENMW_USE_VULKAN
    // GL pixel format values as osg::Image reports them, mapped onto what MyGUI can be handed.
    // Only the formats these interface surfaces actually use are here; anything else is refused
    // rather than reinterpreted, because a wrong guess produces a plausible but wrongly coloured
    // image and nothing reports it.
    bool toMyGuiFormat(GLenum pixelFormat, MyGUI::PixelFormat& out, size_t& bytesPerPixel)
    {
        switch (pixelFormat)
        {
            case GL_RGB:
                out = MyGUI::PixelFormat::R8G8B8;
                bytesPerPixel = 3;
                return true;
            case GL_RGBA:
                out = MyGUI::PixelFormat::R8G8B8A8;
                bytesPerPixel = 4;
                return true;
            case GL_LUMINANCE:
                out = MyGUI::PixelFormat::L8;
                bytesPerPixel = 1;
                return true;
            case GL_LUMINANCE_ALPHA:
                out = MyGUI::PixelFormat::L8A8;
                bytesPerPixel = 2;
                return true;
            default:
                return false;
        }
    }
#endif
}

namespace MWGui
{
    bool usingVulkanGuiPlatform()
    {
#ifdef OPENMW_USE_VULKAN
        return dynamic_cast<VkMyGUIPlatform::RenderManager*>(MyGUI::RenderManager::getInstancePtr()) != nullptr;
#else
        return false;
#endif
    }

    std::unique_ptr<MyGUI::ITexture> createGuiTexture(osg::Texture2D* texture, std::string_view name)
    {
        if (texture == nullptr)
            return nullptr;

        return createGuiTexture(texture, texture->getImage(), name);
    }

    std::unique_ptr<MyGUI::ITexture> createGuiTexture(
        osg::Texture2D* texture, [[maybe_unused]] osg::Image* image, [[maybe_unused]] std::string_view name)
    {
        if (!usingVulkanGuiPlatform())
        {
            if (texture == nullptr)
                return nullptr;
            return std::make_unique<MyGUIPlatform::OSGTexture>(texture);
        }

#ifdef OPENMW_USE_VULKAN
        if (image == nullptr || image->data() == nullptr)
            return nullptr;

        if (image->getDataType() != GL_UNSIGNED_BYTE)
        {
            Log(Debug::Warning) << "Vulkan interface: " << name << " is not 8 bits per channel, leaving it blank";
            return nullptr;
        }

        MyGUI::PixelFormat format = MyGUI::PixelFormat::Unknow;
        size_t bytesPerPixel = 0;
        if (!toMyGuiFormat(image->getPixelFormat(), format, bytesPerPixel))
        {
            Log(Debug::Warning) << "Vulkan interface: " << name << " has an unsupported pixel format 0x" << std::hex
                                << image->getPixelFormat() << ", leaving it blank";
            return nullptr;
        }

        auto result = std::make_unique<VkMyGUIPlatform::Texture>(
            std::string(name), VkMyGUIPlatform::RenderManager::getInstance());
        result->createManual(image->s(), image->t(), MyGUI::TextureUsage::Static | MyGUI::TextureUsage::Write, format);

        unsigned char* dst = static_cast<unsigned char*>(result->lock(MyGUI::TextureUsage::Write));
        const size_t rowBytes = static_cast<size_t>(image->s()) * bytesPerPixel;
        for (int y = 0; y < image->t(); ++y)
        {
            // Row by row, and not flipped. osg::Image's row 0 is the bottom of the picture, and GL
            // samples it at v = 0; a Vulkan texture's row 0 is also what v = 0 samples. Copying in
            // order therefore reproduces exactly what the OSG platform showed, which matters
            // because several of these call sites already flip their UVs to compensate and would
            // otherwise all need changing in step.
            //
            // Row by row rather than one memcpy of the whole thing, because osg pads each row to a
            // four byte boundary: a 21-pixel-wide RGB image carries three bytes of padding per row
            // that must not be copied. data(0, y) starts at the padded row, rowBytes is the
            // unpadded content, and the destination is tightly packed as MyGUI expects.
            std::memcpy(dst + rowBytes * y, image->data(0, y), rowBytes);
        }
        result->unlock();

        return result;
#else
        return nullptr;
#endif
    }
}
