#ifndef OPENMW_COMPONENTS_VK_VKRENDERER_H
#define OPENMW_COMPONENTS_VK_VKRENDERER_H

#include <array>
#include <cstddef>
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
    class Texture;

    // Size of the G-buffer pass's combined-image-sampler array. A fixed-size array is used rather than
    // VK_EXT_descriptor_indexing: the texture index is a push constant and is therefore uniform within
    // a draw call, so plain array indexing in GLSL is legal and no extension is needed. Vulkan requires
    // every element of the array to be written with a valid view, so unused slots point at a 1x1 white
    // fallback texture. Slot 0 is permanently that fallback, so scene texture i lives in slot i + 1.
    constexpr uint32_t maxSceneTextures = 512;

    struct SceneData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewInverse;
        Mat4 projInverse;
        Vec4 sunDirection;
        Vec4 sunColor;
    };

    struct MeshDrawCommand
    {
        VkBuffer vertexBuffer;
        VkBuffer indexBuffer;
        uint32_t indexCount;
        Mat4 transform;
        Mat4 normalMatrix;
        // Device address of this mesh's BLAS, or 0 if it has none. Used to assemble the TLAS.
        VkDeviceAddress blasAddress;
        // Slot in the sampler array declared by the G-buffer fragment shader. 0 is the white fallback.
        uint32_t textureIndex;
    };

    // Layout of the G-buffer pipeline's push constant block. This must match the block declared in
    // gbuffer.vert / gbuffer.frag byte for byte:
    //
    //     offset   0, size 64  mat4 model
    //     offset  64, size 48  mat3 normalMatrix  (std430: 3 columns, each padded out to a vec4)
    //     offset 112, size  4  uint textureIndex
    //     total 116 bytes, within the 128-byte maxPushConstantsSize Vulkan guarantees everywhere.
    //
    // The normal matrix is stored as 12 floats rather than a Mat4 because a push-constant mat3 pads
    // each column to 16 bytes but has no fourth column; using a Mat4 here would shift textureIndex by
    // 16 bytes and silently corrupt it.
    struct GBufferPushConstants
    {
        Mat4 model;
        float normalMatrix[12];
        uint32_t textureIndex;
    };

    static_assert(sizeof(GBufferPushConstants) == 116, "G-buffer push constant layout mismatch");
    static_assert(offsetof(GBufferPushConstants, normalMatrix) == 64, "normalMatrix must be at byte 64");
    static_assert(offsetof(GBufferPushConstants, textureIndex) == 112, "textureIndex must be at byte 112");

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
        void submitMesh(VkBuffer vertexBuffer, VkBuffer indexBuffer, uint32_t indexCount, Mat4 transform,
            VkDeviceAddress blasAddress = 0, uint32_t textureIndex = 0);

        // Rewrites the G-buffer sampler array. views[i] is placed in slot i + 1; slot 0 and any slot
        // left over stay pointed at the white fallback texture. Callers therefore submit meshes with
        // textureIndex = i + 1, or 0 for "untextured". Views beyond maxSceneTextures - 1 are dropped
        // (they keep sampling the fallback) with a single warning rather than a crash.
        void setTextures(const std::vector<VkImageView>& views);

        // Requests a TLAS rebuild on the next frame. The TLAS is only rebuilt when the instance set
        // actually changes (i.e. on cell load/unload) rather than every frame: a rebuild allocates a
        // new acceleration structure and has to idle the device to retire the old one safely.
        void markTlasDirty() { mTlasDirty = true; }

        // Exposed so callers can build GPU resources (e.g. NifVk::MeshConverter) against this device.
        Device& device() { return *mDevice; }
        CommandPool& commandPool() { return *mCommandPool; }

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
        void createTextureSampler();
        void createFallbackTexture();
        void writeTextureArrayDescriptors();
        void createRtOutput();
        void destroyRtOutput();
        void createRtDescriptorSets();
        void buildTlas();
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
        // One per frame in flight, because each references that frame's scene uniform buffer.
        std::array<VkDescriptorSet, maxFramesInFlight> mRtDescriptorSets = {};

        std::array<VkBuffer, maxFramesInFlight> mUniformBuffers = {};
        std::array<VkDeviceMemory, maxFramesInFlight> mUniformMemory = {};
        std::array<void*, maxFramesInFlight> mUniformMapped = {};

        VkSampler mGBufferSampler = VK_NULL_HANDLE;
        // Separate from mGBufferSampler: scene textures want filtering and wrapping, whereas the
        // G-buffer attachments are sampled 1:1 and must not be interpolated or wrapped.
        VkSampler mTextureSampler = VK_NULL_HANDLE;

        // 1x1 opaque white. Populates every otherwise-unused element of the sampler array, which
        // Vulkan requires to be written even if no shader invocation ever reads it.
        std::unique_ptr<Texture> mFallbackTexture;
        // Current contents of the sampler array, always exactly maxSceneTextures entries.
        std::vector<VkImageView> mTextureViews;
        bool mTextureOverflowWarned = false;

        std::vector<VkCommandBuffer> mCommandBuffers;

        std::unique_ptr<RayTracingPipeline> mRtPipeline;
        std::unique_ptr<AccelerationStructure> mTlas;

        std::vector<MeshDrawCommand> mDrawCommands;
        uint32_t mCurrentFrame = 0;
        uint32_t mCurrentImageIndex = 0;
        bool mRayTracingEnabled = false;
        bool mTlasDirty = false;
    };
}

#endif
