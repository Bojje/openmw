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
#include "vkgeometry.hpp"
#include "vkmath.hpp"

// Forward declared rather than including vk_mem_alloc.h, which is a 700 KB single-header library and
// would land in every translation unit that touches a Renderer.
VK_DEFINE_HANDLE(VmaAllocation)

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

    // What a caller hands to submitMesh. Grouped into a struct rather than passed as a parameter list
    // because the ray tracing path needs the buffer device addresses and the alpha-test flag in
    // addition to what rasterization needs, and a nine-argument call is easy to get silently wrong.
    struct MeshSubmission
    {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
        Mat4 transform;
        // Device address of this mesh's BLAS, or 0 if it has none. Used to assemble the TLAS.
        VkDeviceAddress blasAddress = 0;
        // Device addresses of the same vertex and index buffers. The hit shaders read them through
        // buffer references to recover the UV at a hit, which is what makes alpha testing possible.
        VkDeviceAddress vertexAddress = 0;
        VkDeviceAddress indexAddress = 0;
        // Slot in the sampler array declared by the G-buffer fragment shader. 0 is the white fallback.
        uint32_t textureIndex = 0;
        // Whether the silhouette lives in the texture's alpha channel. Drives the any-hit shader's
        // early-out; must agree with the opacity flag the BLAS was built with.
        bool alphaTested = false;
        // Surface response from the NIF material. Defaults are fully rough with no specular, which is
        // what vanilla Morrowind content resolves to -- upstream disables specular outright for
        // Morrowind-era NIFs (nifosg::Loader::applyDrawableProperties).
        float roughness = 1.0f;
        float specularStrength = 0.0f;
    };

    struct MeshDrawCommand
    {
        VkBuffer vertexBuffer;
        VkBuffer indexBuffer;
        uint32_t indexCount;
        Mat4 transform;
        Mat4 normalMatrix;
        VkDeviceAddress blasAddress;
        VkDeviceAddress vertexAddress;
        VkDeviceAddress indexAddress;
        uint32_t textureIndex;
        bool alphaTested;
        float roughness;
        float specularStrength;
    };

    // Layout of the G-buffer pipeline's push constant block. This must match the block declared in
    // gbuffer.vert / gbuffer.frag byte for byte:
    //
    //     offset   0, size 64  mat4 model
    //     offset  64, size 48  mat3 normalMatrix  (std430: 3 columns, each padded out to a vec4)
    //     offset 112, size  4  uint  textureIndex
    //     offset 116, size  4  float roughness
    //     offset 120, size  4  float specularStrength
    //     total 124 bytes, within the 128-byte maxPushConstantsSize Vulkan guarantees everywhere.
    //
    // Only 4 bytes of headroom remain. Anything further -- emissive, a material index -- has to go in a
    // per-instance buffer rather than here.
    //
    // The normal matrix is stored as 12 floats rather than a Mat4 because a push-constant mat3 pads
    // each column to 16 bytes but has no fourth column; using a Mat4 here would shift textureIndex by
    // 16 bytes and silently corrupt it.
    struct GBufferPushConstants
    {
        Mat4 model;
        float normalMatrix[12];
        uint32_t textureIndex;
        float roughness;
        float specularStrength;
    };

    static_assert(sizeof(GBufferPushConstants) == 124, "G-buffer push constant layout mismatch");
    static_assert(sizeof(GBufferPushConstants) <= 128, "exceeds the guaranteed maxPushConstantsSize");
    static_assert(offsetof(GBufferPushConstants, normalMatrix) == 64, "normalMatrix must be at byte 64");
    static_assert(offsetof(GBufferPushConstants, textureIndex) == 112, "textureIndex must be at byte 112");
    static_assert(offsetof(GBufferPushConstants, roughness) == 116, "roughness must be at byte 116");
    static_assert(offsetof(GBufferPushConstants, specularStrength) == 120, "specularStrength at byte 120");

    struct GBufferAttachments
    {
        VkImage albedoImage = VK_NULL_HANDLE;
        VmaAllocation albedoMemory = VK_NULL_HANDLE;
        VkImageView albedoView = VK_NULL_HANDLE;

        VkImage normalImage = VK_NULL_HANDLE;
        VmaAllocation normalMemory = VK_NULL_HANDLE;
        VkImageView normalView = VK_NULL_HANDLE;

        VkImage materialImage = VK_NULL_HANDLE;
        VmaAllocation materialMemory = VK_NULL_HANDLE;
        VkImageView materialView = VK_NULL_HANDLE;

        VkImage depthImage = VK_NULL_HANDLE;
        VmaAllocation depthMemory = VK_NULL_HANDLE;
        VkImageView depthView = VK_NULL_HANDLE;
    };

    struct RtOutputImage
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = VK_NULL_HANDLE;
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
        void submitMesh(const MeshSubmission& submission);

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

        // Blocks until the device is idle. Callers that own GPU resources referenced by submitted
        // command buffers -- cell geometry, terrain -- must call this before destroying them, because
        // up to maxFramesInFlight submissions may still be reading those buffers and BLASes.
        void waitIdle();

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
        // Uploads the per-instance GeometryRecord array the hit shaders index with
        // gl_InstanceCustomIndexEXT, growing the device buffer when the instance count does. Called
        // from buildTlas so the table and the TLAS instance order can never disagree.
        void uploadGeometryTable(const std::vector<GeometryRecord>& records);
        void writeCompositeDescriptor(uint32_t binding, VkImageView view);

        void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage,
            VkImage& image, VmaAllocation& allocation);
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
        std::array<VmaAllocation, maxFramesInFlight> mUniformMemory = {};
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

        // Per-instance GeometryRecord array, parallel to the TLAS instance list. Host visible and
        // rewritten whenever the TLAS is, which is rare (cell load/unload), so a staging copy would
        // buy nothing. Grown geometrically and never shrunk; mGeometryTableCapacity is in records.
        VkBuffer mGeometryTableBuffer = VK_NULL_HANDLE;
        VmaAllocation mGeometryTableMemory = VK_NULL_HANDLE;
        void* mGeometryTableMapped = nullptr;
        uint32_t mGeometryTableCapacity = 0;

        // The scene data last handed to updateScene. Kept CPU-side so the composite pass can recover the
        // camera position without reading back out of the mapped uniform buffer, which is write-combined.
        SceneData mCurrentScene = {};

        std::vector<MeshDrawCommand> mDrawCommands;
        uint32_t mCurrentFrame = 0;
        uint32_t mCurrentImageIndex = 0;
        bool mRayTracingEnabled = false;
        bool mTlasDirty = false;
    };
}

#endif
