#ifndef OPENMW_COMPONENTS_VK_VKRENDERER_H
#define OPENMW_COMPONENTS_VK_VKRENDERER_H

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "vkcommon.hpp"
#include "vkmath.hpp"

struct SDL_Window;

namespace Vk
{
    class Instance;
    class Device;
    class Swapchain;
    class CommandPool;
    class FrameSync;
    class RayTracingPipeline;
    class AccelerationStructure;
    class ShaderModule;

    struct SceneData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewInverse;
        Mat4 projInverse;
        Vec4 sunDirection;
        Vec4 sunColor;
    };

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

    struct RtOutputImage
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    class Renderer
    {
    public:
        Renderer(SDL_Window* window, bool enableValidation);
        ~Renderer();

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        uint32_t beginFrame();
        void endFrame();
        void render();
        void resize(uint32_t width, uint32_t height);
        void cleanup();
        bool loadShadersAndCreatePipelines(const std::string& shaderDir);

        void updateScene(const SceneData& sceneData);

    private:
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
        void createRtOutput();
        void destroyRtOutput();
        void createRtDescriptorSets();
        void writeCompositeDescriptor(uint32_t binding, VkImageView view);

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
        RtOutputImage mRtOutput;

        VkRenderPass mGBufferRenderPass = VK_NULL_HANDLE;
        VkRenderPass mCompositeRenderPass = VK_NULL_HANDLE;

        VkFramebuffer mGBufferFramebuffer = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> mCompositeFramebuffers;

        VkPipeline mGBufferPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mGBufferPipelineLayout = VK_NULL_HANDLE;
        VkPipeline mCompositePipeline = VK_NULL_HANDLE;
        VkPipelineLayout mCompositePipelineLayout = VK_NULL_HANDLE;

        VkDescriptorSetLayout mSceneDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout mCompositeDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout mRtDescriptorLayout = VK_NULL_HANDLE;
        VkDescriptorPool mDescriptorPool = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxFramesInFlight> mSceneDescriptorSets = {};
        std::array<VkDescriptorSet, maxFramesInFlight> mCompositeDescriptorSets = {};
        VkDescriptorSet mRtDescriptorSet = VK_NULL_HANDLE;

        std::array<VkBuffer, maxFramesInFlight> mUniformBuffers = {};
        std::array<VkDeviceMemory, maxFramesInFlight> mUniformMemory = {};
        std::array<void*, maxFramesInFlight> mUniformMapped = {};

        VkSampler mGBufferSampler = VK_NULL_HANDLE;

        std::vector<VkCommandBuffer> mCommandBuffers;

        std::unique_ptr<RayTracingPipeline> mRtPipeline;

        uint32_t mCurrentFrame = 0;
        uint32_t mCurrentImageIndex = 0;
        bool mRayTracingEnabled = false;
    };
}

#endif
