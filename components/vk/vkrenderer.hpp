#ifndef OPENMW_COMPONENTS_VK_VKRENDERER_H
#define OPENMW_COMPONENTS_VK_VKRENDERER_H

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkcommon.hpp"
#include "../render/mesh.hpp"
#include "../render/scene.hpp"
#include "../render/texture.hpp"

struct SDL_Window;

namespace Vk
{
    class Instance;
    class Device;
    class Swapchain;
    class CommandPool;
    class FrameSync;
    class ShaderModule;

    struct GBufferAttachments
    {
        VkImage albedoImage = VK_NULL_HANDLE;
        VkDeviceMemory albedoMemory = VK_NULL_HANDLE;
        VkImageView albedoView = VK_NULL_HANDLE;

        VkImage normalImage = VK_NULL_HANDLE;
        VkDeviceMemory normalMemory = VK_NULL_HANDLE;
        VkImageView normalView = VK_NULL_HANDLE;

        VkImage materialImage = VK_NULL_HANDLE;
        VkDeviceMemory materialMemory = VK_NULL_HANDLE;
        VkImageView materialView = VK_NULL_HANDLE;

        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
    };

    class Renderer
    {
    public:
        using TextureResolver = Render::TextureResolver;

        Renderer(SDL_Window* window, bool enableValidation);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        bool beginFrame();
        void endFrame();
        bool render();
        void resize(uint32_t width, uint32_t height);
        void cleanup();
        bool loadShadersAndCreatePipelines(const std::string& shaderDir);

        void updateScene(const Render::SceneData& sceneData);
        void setMeshes(const std::vector<Render::MeshInstance>& meshes, TextureResolver textureResolver = {});

    private:
        static constexpr uint32_t maxTextures = 64;

        struct TextureResource
        {
            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
        };

        void createSurface();
        void createGBuffer();
        void destroyGBuffer();
        void createGBufferRenderPass();
        void createCompositeRenderPass();
        void createGBufferFramebuffer();
        void createCompositeFramebuffers();
        void destroyCompositeFramebuffers();
        void createGBufferPipeline();
        void createCompositePipeline();
        void createDescriptorSetLayouts();
        void createDescriptorPool();
        void createDescriptorSets();
        void createUniformBuffers();
        void createGBufferSampler();
        void writeCompositeDescriptor(uint32_t binding, VkImageView view);
        void writeSceneTextureDescriptor(uint32_t textureIndex, VkImageView view);
        uint32_t createTextureResource(const Render::TextureData& texture);
        void destroyTextures();
        void destroyMesh();

        void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
            VkImage& image, VkDeviceMemory& memory);
        VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
        void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
            VkImageLayout newLayout, VkImageAspectFlags aspectMask);
        VkFormat findDepthFormat();

        SDL_Window* mWindow;
        VkSurfaceKHR mSurface = VK_NULL_HANDLE;

        std::unique_ptr<Instance> mInstance;
        std::unique_ptr<Device> mDevice;
        std::unique_ptr<Swapchain> mSwapchain;
        std::unique_ptr<CommandPool> mCommandPool;
        std::unique_ptr<FrameSync> mFrameSync;

        GBufferAttachments mGBuffer;

        VkRenderPass mGBufferRenderPass = VK_NULL_HANDLE;
        VkRenderPass mCompositeRenderPass = VK_NULL_HANDLE;

        VkFramebuffer mGBufferFramebuffer = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> mCompositeFramebuffers;

        VkPipeline mGBufferPipeline = VK_NULL_HANDLE;
        VkPipeline mGBufferAlphaPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mGBufferPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mCompositePipeline = VK_NULL_HANDLE;
        VkPipelineLayout mCompositePipelineLayout = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSceneDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout mCompositeDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxFramesInFlight> mSceneDescriptorSets = {};
        std::array<VkDescriptorSet, maxFramesInFlight> mCompositeDescriptorSets = {};

        std::array<VkBuffer, maxFramesInFlight> mUniformBuffers = {};
        std::array<VkDeviceMemory, maxFramesInFlight> mUniformMemory = {};
        std::array<void*, maxFramesInFlight> mUniformMapped = {};
        Render::SceneData mSceneData = {};
        bool mHasSceneData = false;

        VkSampler mGBufferSampler = VK_NULL_HANDLE;

        VkCommandBuffer mUploadCommandBuffer = VK_NULL_HANDLE;
        std::vector<TextureResource> mTextures;
        std::unordered_map<std::string, uint32_t> mTextureIndices;
        std::vector<uint32_t> mMeshTextureIndices;

        VkBuffer mMeshVertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mMeshVertexMemory = VK_NULL_HANDLE;
        VkBuffer mMeshIndexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory mMeshIndexMemory = VK_NULL_HANDLE;

        std::vector<Render::MeshDraw> mMeshDraws;

        std::vector<VkCommandBuffer> mCommandBuffers;

        uint32_t mCurrentFrame = 0;
        uint32_t mCurrentImageIndex = 0;
    };
}

#endif
