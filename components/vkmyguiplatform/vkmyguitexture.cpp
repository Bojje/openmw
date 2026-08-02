#include "vkmyguitexture.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <osg/Image>

#include <components/debug/debuglog.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/vk/vkcommands.hpp>
#include <components/vk/vkdevice.hpp>
#include <components/vk/vkrenderer.hpp>

#include "vkmyguirendermanager.hpp"

namespace VkMyGUIPlatform
{
    namespace
    {
        // GL pixel format values as reported by osg::Image::getPixelFormat(), spelled out so this
        // file needs no GL header. The same trick, and the same values where they overlap, as
        // apps/openmw/mwrender/vkrenderingmanager.cpp.
        constexpr unsigned int sGlAlpha = 0x1906;
        constexpr unsigned int sGlRgb = 0x1907;
        constexpr unsigned int sGlRgba = 0x1908;
        constexpr unsigned int sGlLuminance = 0x1909;
        constexpr unsigned int sGlLuminanceAlpha = 0x190A;
        constexpr unsigned int sGlBgr = 0x80E0;
        constexpr unsigned int sGlBgra = 0x80E1;
        constexpr unsigned int sGlDxt1Rgb = 0x83F0;
        constexpr unsigned int sGlDxt1Rgba = 0x83F1;
        constexpr unsigned int sGlDxt3 = 0x83F2;
        constexpr unsigned int sGlDxt5 = 0x83F3;

        // _SRGB rather than _UNORM throughout, for the reason set out in gui.frag: the swapchain is
        // an sRGB format and applies the transfer function on store, so a texture sampled without
        // decoding would be encoded twice and the whole interface would come out pale.
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
                default: return VK_FORMAT_UNDEFINED;
            }
        }

        // Expands a format Vulkan either lacks or barely supports into RGBA8.
        //
        // Three-channel sampled images are optional in Vulkan and absent on plenty of drivers, and
        // there is no luminance format at all -- the closest equivalent needs a component swizzle on
        // the image view, which Vk::Texture does not expose. Expanding costs one pass over an
        // interface texture, which happens once at load.
        //
        // Returns false for a format this does not know, which the caller treats as "cannot upload".
        bool expandToRgba(const unsigned char* src, unsigned int glPixelFormat, size_t pixels,
            std::vector<unsigned char>& out)
        {
            out.resize(pixels * 4);

            for (size_t i = 0; i < pixels; ++i)
            {
                unsigned char* dst = out.data() + i * 4;
                switch (glPixelFormat)
                {
                    case sGlRgb:
                        dst[0] = src[i * 3 + 0];
                        dst[1] = src[i * 3 + 1];
                        dst[2] = src[i * 3 + 2];
                        dst[3] = 255;
                        break;
                    case sGlBgr:
                        dst[0] = src[i * 3 + 2];
                        dst[1] = src[i * 3 + 1];
                        dst[2] = src[i * 3 + 0];
                        dst[3] = 255;
                        break;
                    case sGlLuminance:
                        dst[0] = dst[1] = dst[2] = src[i];
                        dst[3] = 255;
                        break;
                    case sGlLuminanceAlpha:
                        dst[0] = dst[1] = dst[2] = src[i * 2 + 0];
                        dst[3] = src[i * 2 + 1];
                        break;
                    // Alpha-only is white with the source as coverage, which is what GL_ALPHA
                    // combined with the fixed-function pipeline's default modulate produced.
                    case sGlAlpha:
                        dst[0] = dst[1] = dst[2] = 255;
                        dst[3] = src[i];
                        break;
                    default:
                        return false;
                }
            }

            return true;
        }

        // MyGUI's own pixel formats, for the createManual/lock/unlock path. Same expansion argument
        // as above; OpenMW only ever asks for R8G8B8 and R8G8B8A8, but L8 and L8A8 are part of the
        // interface and cost four lines each.
        bool expandMyGuiFormat(const unsigned char* src, MyGUI::PixelFormat format, size_t pixels,
            std::vector<unsigned char>& out)
        {
            switch (format.getValue())
            {
                case MyGUI::PixelFormat::L8: return expandToRgba(src, sGlLuminance, pixels, out);
                case MyGUI::PixelFormat::L8A8: return expandToRgba(src, sGlLuminanceAlpha, pixels, out);
                case MyGUI::PixelFormat::R8G8B8: return expandToRgba(src, sGlRgb, pixels, out);
                case MyGUI::PixelFormat::R8G8B8A8:
                    out.assign(src, src + pixels * 4);
                    return true;
                default: return false;
            }
        }
    }

    Texture::Texture(std::string name, RenderManager& manager)
        : mName(std::move(name))
        , mManager(&manager)
        , mFormat(MyGUI::PixelFormat::Unknow)
        , mUsage(MyGUI::TextureUsage::Default)
    {
    }

    Texture::~Texture()
    {
        destroy();
    }

    void Texture::destroy()
    {
        mManager->retireTexture(std::move(mTexture), mDescriptorSet);
        mTexture.reset();
        mDescriptorSet = VK_NULL_HANDLE;

        mLockedData.clear();
        mLockedData.shrink_to_fit();
        mLocked = false;

        mFormat = MyGUI::PixelFormat::Unknow;
        mUsage = MyGUI::TextureUsage::Default;
        mNumElemBytes = 0;
        mWidth = 0;
        mHeight = 0;
    }

    void Texture::upload(
        const void* data, VkDeviceSize size, uint32_t width, uint32_t height, VkFormat format, uint32_t levels)
    {
        Vk::Renderer& renderer = mManager->renderer();

        auto texture = std::make_unique<Vk::Texture>(Vk::Texture::create(
            renderer.device(), renderer.commandPool(), width, height, format, data, size, levels));
        VkDescriptorSet set = mManager->acquireTextureSet(texture->view());

        // A new image and a new set rather than a copy into the existing ones. Overwriting either in
        // place would race a command buffer that has already bound them, and MyGUI's lock/unlock is
        // rare enough -- the font atlas once at startup, three 8x8 fills, the menu transparency when
        // the setting changes -- that the reuse is not worth the synchronisation it would need.
        mManager->retireTexture(std::move(mTexture), mDescriptorSet);

        mTexture = std::move(texture);
        mDescriptorSet = set;
        mWidth = static_cast<int>(width);
        mHeight = static_cast<int>(height);
    }

    void Texture::createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format)
    {
        size_t elemBytes = 0;
        switch (format.getValue())
        {
            case MyGUI::PixelFormat::L8: elemBytes = 1; break;
            case MyGUI::PixelFormat::L8A8: elemBytes = 2; break;
            case MyGUI::PixelFormat::R8G8B8: elemBytes = 3; break;
            case MyGUI::PixelFormat::R8G8B8A8: elemBytes = 4; break;
            default: break;
        }

        if (elemBytes == 0)
            throw std::runtime_error("Texture format not supported");

        mFormat = format;
        mUsage = usage;
        mNumElemBytes = elemBytes;
        mWidth = width;
        mHeight = height;

        // No image is created here, unlike a Vulkan resource's usual lifetime, because there is
        // nothing to put in it: the caller's next act is lock/write/unlock. Until that happens
        // descriptorSet() stays null and the render manager substitutes its white fallback, which is
        // the correct appearance for a texture whose contents do not exist yet.
    }

    void* Texture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (mNumElemBytes == 0 || mWidth <= 0 || mHeight <= 0)
            throw std::runtime_error("Texture is not created");
        if (mLocked)
            throw std::runtime_error("Texture already locked");

        // Zeroed rather than seeded with the current contents. Reading a texture back would mean a
        // host-visible copy of every interface texture for a case MyGUI does not have: every caller
        // in OpenMW writes the whole surface. TextureUsage::Read is accordingly not honoured.
        mLockedData.assign(static_cast<size_t>(mWidth) * mHeight * mNumElemBytes, 0);
        mLocked = true;

        return mLockedData.data();
    }

    void Texture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("Texture not locked");

        mLocked = false;

        std::vector<unsigned char> rgba;
        const size_t pixels = static_cast<size_t>(mWidth) * mHeight;
        if (!expandMyGuiFormat(mLockedData.data(), mFormat, pixels, rgba))
        {
            Log(Debug::Warning) << "Vulkan MyGUI: unsupported pixel format for texture " << mName;
            return;
        }

        upload(rgba.data(), rgba.size(), static_cast<uint32_t>(mWidth), static_cast<uint32_t>(mHeight),
            VK_FORMAT_R8G8B8A8_SRGB, 1);

        mLockedData.clear();
        mLockedData.shrink_to_fit();
    }

    void Texture::loadFromFile(const std::string& fname)
    {
        Resource::ImageManager* imageManager = mManager->imageManager();
        if (imageManager == nullptr)
            throw std::runtime_error("No imagemanager set");

        // The path is used as given, matching the OSG platform: MyGUI resolves skin resources through
        // the data manager and hands over a name the VFS already understands, so
        // correctTexturePath -- which exists for Morrowind's implicit textures/ prefix and its
        // .tga-means-.dds substitution -- would be wrong here rather than merely redundant.
        osg::ref_ptr<osg::Image> image = imageManager->getImage(VFS::Path::Normalized(fname));
        if (image == nullptr || !image->valid() || image->data() == nullptr)
        {
            Log(Debug::Warning) << "Vulkan MyGUI: could not load texture " << fname;
            return;
        }

        const uint32_t width = static_cast<uint32_t>(image->s());
        const uint32_t height = static_cast<uint32_t>(image->t());
        const unsigned int glFormat = image->getPixelFormat();

        // Level 0 only, no matter what the file carries. Interface textures are drawn at or near 1:1
        // and the sampler has no mip mode, so a chain would be uploaded and never selected.
        VkFormat format = toVkFormat(glFormat);
        if (format != VK_FORMAT_UNDEFINED)
        {
            const VkDeviceSize size = Vk::Texture::levelSizeInBytes(format, width, height);
            if (size == 0)
            {
                Log(Debug::Warning) << "Vulkan MyGUI: cannot size texture " << fname;
                return;
            }
            upload(image->data(), size, width, height, format, 1);
        }
        else
        {
            std::vector<unsigned char> rgba;
            if (!expandToRgba(image->data(), glFormat, static_cast<size_t>(width) * height, rgba))
            {
                Log(Debug::Warning) << "Vulkan MyGUI: unsupported pixel format " << glFormat
                                    << " for texture " << fname;
                return;
            }
            upload(rgba.data(), rgba.size(), width, height, VK_FORMAT_R8G8B8A8_SRGB, 1);
        }

        mFormat = MyGUI::PixelFormat::R8G8B8A8;
        mNumElemBytes = 4;
        mUsage = MyGUI::TextureUsage::Static;
    }

    void Texture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "Would save image to file " << fname;
    }

    MyGUI::IRenderTarget* Texture::getRenderTarget()
    {
        // Render to texture is not implemented, exactly as in the OSG platform. Nothing in OpenMW
        // asks MyGUI for one; the render-to-texture surfaces the interface does show -- the character
        // preview, the maps, video -- are produced outside MyGUI and handed in as ready textures.
        return nullptr;
    }

    void Texture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "VkMyGUIPlatform::Texture::setShader is not implemented";
    }

}
