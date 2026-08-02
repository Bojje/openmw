#ifndef OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIRENDERMANAGER_H
#define OPENMW_COMPONENTS_VKMYGUIPLATFORM_VKMYGUIRENDERMANAGER_H

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <MyGUI_RenderManager.h>

#include <vulkan/vulkan.h>

#include <components/vk/vkbuffer.hpp>
#include <components/vk/vkcommon.hpp>
#include <components/vk/vktexture.hpp>

namespace Resource
{
    class ImageManager;
}

namespace Vk
{
    class Renderer;
}

namespace VkMyGUIPlatform
{
    class Texture;

    /// Which of the two blend pipelines a draw call uses.
    ///
    /// MyGUI's platform interface has no concept of a blend mode -- IRenderTarget::doRender takes a
    /// buffer, a texture and a count and nothing else. OpenMW nevertheless needs an additive layer for
    /// glows and magic effects, and the OSG platform gets it by letting a layer inject an
    /// osg::StateSet that doRender then pushes. There is no equivalent to inject here, so the same
    /// need is met by selecting between two pipelines that differ only in their blend factors.
    enum class BlendMode
    {
        Alpha = 0,
        Additive = 1
    };

    /// MyGUI's rendering platform implemented against Vk::Renderer.
    ///
    /// The sibling of components/myguiplatform, and deliberately structured the same way so the two
    /// can be read side by side. What differs is where the draw calls land: the OSG platform collects
    /// batches during the cull traversal and replays them from an osg::Drawable during the draw
    /// traversal, because OSG's draw thread runs concurrently with the next frame's update. There is
    /// no such thread here. The renderer calls back into collectDrawCalls() while it is recording the
    /// composite pass, so doRender records straight into that command buffer and no batch list, double
    /// buffering or deferral is needed for correctness.
    ///
    /// What still needs deferral is destruction. A texture or vertex buffer released this frame may
    /// be referenced by up to maxFramesInFlight command buffers that have not completed, so
    /// everything is retired through a queue and freed once the frame that could still be reading it
    /// has retired.
    class RenderManager final : public MyGUI::RenderManager, public MyGUI::IRenderTarget
    {
    public:
        RenderManager(Vk::Renderer& renderer, Resource::ImageManager* imageManager, float scalingFactor);
        ~RenderManager() override;

        RenderManager(const RenderManager&) = delete;
        RenderManager& operator=(const RenderManager&) = delete;

        void initialise();
        void shutdown();

        /// Builds the two blend pipelines from gui.vert.spv and gui.frag.spv in \a shaderDir. Returns
        /// false and leaves the pipelines null if either is missing, in which case doRender records
        /// nothing -- the same failure shape Vk::Renderer::loadShadersAndCreatePipelines has, so a
        /// missing shader directory produces a warning rather than a crash.
        bool loadShaders(const std::string& shaderDir);

        static RenderManager& getInstance() { return *getInstancePtr(); }
        static RenderManager* getInstancePtr()
        {
            return static_cast<RenderManager*>(MyGUI::RenderManager::getInstancePtr());
        }

        bool checkTexture(MyGUI::ITexture* texture) override;

        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;

        MyGUI::IVertexBuffer* createVertexBuffer() override;
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;

        MyGUI::ITexture* createTexture(const std::string& name) override;
        void destroyTexture(MyGUI::ITexture* texture) override;
        MyGUI::ITexture* getTexture(const std::string& name) override;

        void begin() override;
        void end() override;
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;

        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

        void setViewSize(int width, int height) override;

        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        /// Selects the pipeline subsequent draws use, until it is set back. Called by AdditiveLayer
        /// around its base-class render, mirroring the OSG platform's setInjectState.
        void setBlendMode(BlendMode mode) { mBlendMode = mode; }

        /*internal:*/

        Vk::Renderer& renderer() { return mRenderer; }
        Resource::ImageManager* imageManager() { return mImageManager; }

        /// A descriptor set holding \a view, ready to bind. Sets are recycled rather than freed: they
        /// all share one layout, there are only ever a few hundred, and reusing one avoids needing a
        /// pool created with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT and the per-pool
        /// bookkeeping that goes with it. A recycled set is only handed back out after the deferred
        /// delay below, so writing it can never disturb a set a pending command buffer has bound.
        VkDescriptorSet acquireTextureSet(VkImageView view);

        /// Queues a texture and its descriptor set for destruction once no in-flight frame can still
        /// be reading them. Either may be null.
        void retireTexture(std::unique_ptr<Vk::Texture> texture, VkDescriptorSet set);

        /// Queues a vertex buffer for destruction on the same terms.
        void retireBuffer(std::unique_ptr<Vk::Buffer> buffer);

    private:
        void createDescriptorLayout();
        void createSampler();
        void createFallbackTexture();
        void destroyPipelines();
        void collectRetired(bool force);
        VkDescriptorSet allocateSet();

        /// Called by the renderer from inside the composite render pass.
        void recordFrame(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent);

        Vk::Renderer& mRenderer;
        Resource::ImageManager* mImageManager;
        float mInvScalingFactor;

        MyGUI::IntSize mViewSize;
        MyGUI::RenderTargetInfo mInfo;
        MyGUI::VertexColourType mVertexFormat;
        bool mUpdate = false;
        bool mIsInitialise = false;

        // Held by value behind a unique_ptr rather than by value directly, unlike the OSG platform's
        // std::map<std::string, OSGTexture>: MyGUI hands out raw ITexture* that outlive any rehash or
        // reassignment of the container, and a unique_ptr makes that stability unconditional.
        std::map<std::string, std::unique_ptr<Texture>> mTextures;

        VkDescriptorSetLayout mDescriptorLayout = VK_NULL_HANDLE;
        VkPipelineLayout mPipelineLayout = VK_NULL_HANDLE;
        std::array<VkPipeline, 2> mPipelines = { VK_NULL_HANDLE, VK_NULL_HANDLE };
        VkSampler mSampler = VK_NULL_HANDLE;

        std::vector<VkDescriptorPool> mDescriptorPools;
        std::vector<VkDescriptorSet> mFreeSets;

        // 1x1 opaque white, bound for any draw whose texture is null or has no image yet. MyGUI
        // creates a texture through createManual and only fills it on the first unlock, so the
        // untextured case is real and happens during startup rather than never.
        std::unique_ptr<Vk::Texture> mFallbackTexture;
        VkDescriptorSet mFallbackSet = VK_NULL_HANDLE;

        // Valid only while the renderer is recording the composite pass.
        VkCommandBuffer mCommandBuffer = VK_NULL_HANDLE;
        VkExtent2D mExtent = {};
        BlendMode mBlendMode = BlendMode::Alpha;
        VkPipeline mBoundPipeline = VK_NULL_HANDLE;

        // Monotonic count of recorded frames, which is what the retirement queue ages against. Not
        // the renderer's frame-in-flight index: that alternates and cannot express "two frames ago".
        uint64_t mFrameCounter = 0;

        struct Retired
        {
            uint64_t frame;
            std::unique_ptr<Vk::Texture> texture;
            std::unique_ptr<Vk::Buffer> buffer;
            VkDescriptorSet set;
        };

        std::vector<Retired> mRetired;
    };

}

#endif
