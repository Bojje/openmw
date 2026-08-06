#ifndef OPENMW_COMPONENTS_VK_VKRENDERER_H
#define OPENMW_COMPONENTS_VK_VKRENDERER_H

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkcommon.hpp"
#include "../render/mesh.hpp"
#include "../render/scene.hpp"
#include "../render/submission.hpp"
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
        enum class SurfaceMode
        {
            Window,
            Headless,
        };

        Renderer(SDL_Window* window, bool enableValidation, SurfaceMode surfaceMode = SurfaceMode::Window,
            uint32_t width = 640, uint32_t height = 480);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        bool render();
        std::optional<Render::TextureData> captureFrame();
        void resize(uint32_t width, uint32_t height);
        bool loadShadersAndCreatePipelines(const std::string& shaderDir);
        bool validationEnabled() const;
        uint32_t validationErrorCount() const;

        void setScene(const Render::SceneSubmission& submission);

    private:
        static constexpr uint32_t maxTextures = maxSceneTextures;

        struct TextureResource
        {
            enum class SamplerMode
            {
                Repeat,
                Clamp,
                RepeatU,
                RepeatV,
            };

            VkImage image = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            VkImageView view = VK_NULL_HANDLE;
            SamplerMode samplerMode = SamplerMode::Repeat;
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
        void writeSceneTextureDescriptor(uint32_t frameIndex, uint32_t binding, uint32_t textureIndex, VkImageView view);
        void syncSceneTextureDescriptors(uint32_t frameIndex);
        uint32_t createTextureResource(const Render::TextureData& texture,
            TextureResource::SamplerMode samplerMode = TextureResource::SamplerMode::Repeat);
        void destroyTextures();
        void destroyMesh();
        void destroyMesh(uint32_t frameIndex);
        void uploadMesh(uint32_t frameIndex);
        void setMeshes(const std::vector<Render::MeshInstance>& meshes, Render::TextureResolver textureResolver);

        void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
            VkImage& image, VkDeviceMemory& memory);
        bool beginFrame();
        bool endFrame();
        void cleanup();
        VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags);
        void transitionImageLayout(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout,
            VkImageLayout newLayout, VkImageAspectFlags aspectMask);
        VkFormat findDepthFormat();

        SDL_Window* mWindow;
        bool mHeadless = false;
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
        VkPipeline mGBufferDoubleSidedPipeline = VK_NULL_HANDLE;
        VkPipeline mGBufferAlphaPipeline = VK_NULL_HANDLE;
        VkPipeline mGBufferAlphaDoubleSidedPipeline = VK_NULL_HANDLE;
        VkPipeline mGBufferTerrainFirstPipeline = VK_NULL_HANDLE;
        VkPipeline mGBufferTerrainLayerPipeline = VK_NULL_HANDLE;
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
        VkSampler mSceneSampler = VK_NULL_HANDLE;
        VkSampler mAlphaSampler = VK_NULL_HANDLE;
        VkSampler mRepeatUSampler = VK_NULL_HANDLE;
        VkSampler mRepeatVSampler = VK_NULL_HANDLE;

        VkCommandBuffer mUploadCommandBuffer = VK_NULL_HANDLE;
        std::vector<TextureResource> mTextures;
        std::unordered_map<std::string, uint32_t> mTextureIndices;
        std::vector<uint32_t> mMeshTextureIndices;
        std::vector<uint32_t> mMeshAlphaTextureIndices;
        std::vector<uint32_t> mMeshNormalTextureIndices;
        std::vector<uint32_t> mMeshEmissiveTextureIndices;

        struct MeshBuffers
        {
            VkBuffer vertex = VK_NULL_HANDLE;
            VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
            VkBuffer index = VK_NULL_HANDLE;
            VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        };

        std::array<MeshBuffers, maxFramesInFlight> mMeshBuffers = {};
        std::vector<Render::MeshVertex> mMeshVertices;
        std::vector<uint32_t> mMeshIndices;
        uint64_t mMeshRevision = 1;
        std::array<uint64_t, maxFramesInFlight> mUploadedMeshRevisions = {};

        std::vector<Render::MeshDraw> mMeshDraws;

        std::vector<VkCommandBuffer> mCommandBuffers;

        uint32_t mCurrentFrame = 0;
        uint32_t mCurrentImageIndex = 0;
        uint32_t mLastSubmittedFrame = 0;
        uint32_t mLastSubmittedImage = 0;
        bool mHasSubmittedFrame = false;
    };
}

#endif
