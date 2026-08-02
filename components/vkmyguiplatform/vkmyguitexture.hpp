#ifndef OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUITEXTURE_H
#define OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUITEXTURE_H

#include <memory>
#include <string>
#include <vector>

#include <MyGUI_ITexture.h>

#include <vulkan/vulkan.h>

#include <components/vk/vktexture.hpp>

namespace VkMyGUIPlatform
{
    class RenderManager;

    /// MyGUI::ITexture backed by a Vk::Texture and a descriptor set holding its view.
    ///
    /// The descriptor set is owned per texture rather than written per draw. MyGUI issues one
    /// doRender per texture change and a busy interface has a few hundred of them, so writing a
    /// descriptor set that often would mean either a per-frame ring of sets or
    /// VK_KHR_push_descriptor, and neither buys anything over the set simply living as long as the
    /// image it points at.
    class Texture final : public MyGUI::ITexture
    {
    public:
        Texture(std::string name, RenderManager& manager);
        ~Texture() override;

        Texture(const Texture&) = delete;
        Texture& operator=(const Texture&) = delete;

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;

        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return mLocked; }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        size_t getNumElemBytes() const override { return mNumElemBytes; }

        MyGUI::IRenderTarget* getRenderTarget() override;

        void setShader(const std::string& shaderName) override;

        /*internal:*/

        /// Null until the first successful upload. A draw that gets null falls back to the render
        /// manager's white texture rather than being skipped, because a widget with no texture is
        /// still meant to show its vertex colour.
        VkDescriptorSet descriptorSet() const { return mDescriptorSet; }

    private:
        /// Replaces the image and the descriptor set, retiring whatever was there. \a data is a mip
        /// chain of \a levels levels laid out back to back, exactly as Vk::Texture::create expects.
        void upload(const void* data, VkDeviceSize size, uint32_t width, uint32_t height, VkFormat format,
            uint32_t levels);

        std::string mName;
        RenderManager* mManager;

        std::unique_ptr<Vk::Texture> mTexture;
        VkDescriptorSet mDescriptorSet = VK_NULL_HANDLE;

        // Staging for lock/unlock. MyGUI writes pixels into this and unlock uploads them; there is no
        // path that reads back, and TextureUsage::Read is not supported.
        std::vector<unsigned char> mLockedData;
        bool mLocked = false;

        MyGUI::PixelFormat mFormat;
        MyGUI::TextureUsage mUsage;
        size_t mNumElemBytes = 0;

        int mWidth = 0;
        int mHeight = 0;
    };

}

#endif
