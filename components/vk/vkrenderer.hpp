#ifndef OPENMW_COMPONENTS_VK_VKRENDERER_H
#define OPENMW_COMPONENTS_VK_VKRENDERER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
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
    class Buffer;
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

    // Point lights visible to the composite pass in one frame. Morrowind interiors are dense with
    // torches and candles but the active cell set is small, so this is generous; the collector already
    // culls by distance. A fixed-size per-frame buffer avoids reallocating every frame.
    constexpr uint32_t maxPointLights = 256;

    struct SceneData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewInverse;
        Mat4 projInverse;
        Vec4 sunDirection;
        Vec4 sunColor;
        // Effective ambient for the current cell, already decoded to linear. This is Morrowind's
        // authored per-cell mood colour, which is most of what gives an interior its identity -- a
        // flat grey constant here throws that away and makes every cell in the game look the same.
        Vec4 ambientColor;
        // Sky and fog colour for the current weather and hour, decoded to linear. OpenMW's entire
        // atmosphere model is a lerp between these two along the vertical: the sky dome is one flat
        // colour whose alpha ramps to zero at the horizon, over a clear colour set to the fog colour.
        // Reusing the same fog colour for both is why its horizon reads as continuous.
        Vec4 skyColor;
        Vec4 fogColor;
        // x = fog start, y = fog end, both in world units along the view axis. OpenMW's default fog is
        // planar and linear: clamp((abs(viewZ) - start) / (end - start), 0, 1).
        Vec4 fogParams;
        // Rigid transform from this frame's view space to last frame's view space, i.e.
        // prevView * inverse(curView). Used for temporal reprojection.
        //
        // Deliberately not the more obvious prevProjection * prevView. Morrowind's world runs to
        // +/-250,000 units and fp32 has a 24-bit mantissa, so ulp(250000) is 1/32 of a world unit.
        // Reprojecting via a previous view-projection stacks two cancellations of that size: the
        // world position is reconstructed through viewInverse, whose translation column is the camera
        // position, and is then multiplied by a matrix whose translation is -R*camPos. Both operands
        // are ~250,000 and the result's w is the distance to the surface, tens of units -- a 5000x
        // cancellation, which comes out as a systematic reprojection smear that worsens the further
        // east the player walks.
        //
        // This formulation never forms that product. Its rotation block is R_prev * transpose(R_cur),
        // every entry at most 1, and its translation is one frame of camera motion -- tens of units.
        // Last frame's *projection* is deliberately not carried: it only changes on a resize or an FOV
        // change, and both of those invalidate the history anyway.
        //
        // No motion vector G-buffer target is needed for this, and that is worth understanding before
        // anyone adds one: every instance in the TLAS is static world geometry, because markTlasDirty
        // only fires on cell load and actors are not in the acceleration structure at all. With a
        // static scene the reprojection is exact rather than approximate. The moment moving objects
        // enter the TLAS that stops being true and real motion vectors become necessary.
        Mat4 prevViewFromCurView;
        // Temporal accumulator tuning, read by raygen.rgen.
        //   x = alphaMin, the floor on the exponential blend weight; 1/x is the longest the filter can
        //       take to respond to a real change, so it bounds ghosting by construction.
        //   y = relative depth tolerance for history rejection.
        //   z = minimum dot(N, storedN) for history rejection.
        //   w = maximum history length in frames. Zero means "discard all history this frame", which
        //       is how a TLAS rebuild invalidates the accumulator -- see Renderer::updateScene.
        Vec4 denoiseParams;
        // x = cosine of the sun's angular radius, which is what turns the shadow term from a delta
        // light into an area light: the shadow ray is jittered within that cone, so a surface partly
        // occluded across the sun's disc resolves to a partial value instead of stepping straight
        // from lit to unlit. Exactly 1.0 reproduces the old hard shadow bit for bit, which is what
        // makes the pre-existing behaviour the ground truth for this change.
        //
        // Everything past x is spare. The sun's true angular radius is about 0.27 degrees; the value
        // actually used is larger, because it is standing in for the softening that a real sky
        // radiance model and an ambient occlusion term would otherwise provide.
        Vec4 sunParams;
        // Frames rendered so far, for jittered sampling sequences and for deciding how much history a
        // temporal accumulator may trust. Wraps; only ever used modulo something small.
        uint32_t frameIndex = 0;
        // How many entries of the point light buffer are live this frame.
        uint32_t lightCount = 0;
        // Non-zero in an interior. The ambient colour means something different there: outdoors it is
        // sky light, which the ray traced sky visibility term is a correct occlusion factor for, but
        // indoors it is Morrowind's authored per-cell fill and nothing escapes to sky, so that term
        // is near zero everywhere and multiplying by it cancels the only light the cell has.
        //
        // Occupies what was scenePad0, so the block's layout does not change. Adding a field instead
        // would shift everything after it in two shaders that nothing cross-checks -- trap 18.
        uint32_t isInterior = 0;
        uint32_t scenePad1 = 0;
    };

    // std140 requires a vec4 to start on a 16-byte boundary, and the shader declarations in
    // raygen.rgen and composite.frag mirror this struct field for field. Nothing validates that
    // agreement, so pin the offsets that would silently shift if a field were inserted or resized.
    static_assert(offsetof(SceneData, prevViewFromCurView) == 352, "SceneData layout drifted");
    static_assert(offsetof(SceneData, denoiseParams) == 416, "denoiseParams must be 16-byte aligned");
    static_assert(offsetof(SceneData, sunParams) == 432, "sunParams must be 16-byte aligned");
    static_assert(offsetof(SceneData, frameIndex) == 448, "SceneData layout drifted");
    static_assert(sizeof(SceneData) == 464, "SceneData layout drifted");

    // Mirrors MWRender::VkPointLight. 64 bytes, std430-compatible, so the collector's vector memcpys
    // straight into the buffer. Declared here rather than shared with the apps layer because
    // components must not depend on apps; the static_asserts below are the contract.
    struct PointLight
    {
        float position[3];
        float radius;
        float diffuse[3];
        float attenuationConstant;
        float ambient[3];
        float attenuationLinear;
        float specular[3];
        float attenuationQuadratic;
    };

    static_assert(sizeof(PointLight) == 64, "PointLight must match the std430 layout in composite.frag");
    static_assert(offsetof(PointLight, diffuse) == 16, "PointLight layout drifted from the shader");
    static_assert(offsetof(PointLight, ambient) == 32, "PointLight layout drifted from the shader");
    static_assert(offsetof(PointLight, specular) == 48, "PointLight layout drifted from the shader");

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
        // The shape's skin, or nothing. Both must be set for the skinned pipeline to be used: the
        // buffer supplies four bone indices and four weights per vertex, and boneOffset says where
        // this instance's bone palette starts in the matrices handed to updateSkinMatrices. The
        // indices in the buffer are relative to that offset.
        //
        // A shape with a skin buffer but no palette this frame -- the palette overflowed, or the
        // caller chose not to build one -- draws through the rigid pipeline in bind pose.
        VkBuffer skinBuffer = VK_NULL_HANDLE;
        uint32_t boneOffset = sNoBones;

        // Whether this instance survives frustum culling.
        //
        // It gates the *raster* pass only. The TLAS deliberately ignores it: an object behind the
        // camera still casts a shadow into view, and a reflection ray can hit anything at all. Culling
        // the acceleration structure would make shadows pop in and out as the camera turns, which is a
        // far worse artifact than the draw call it saves.
        bool visible = true;
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
        bool visible;
        // At the end, and new fields must stay at the end: submitMesh builds this with a positional
        // aggregate initialiser, so a field inserted in the middle silently shifts everything after
        // it onto the wrong member.
        VkBuffer skinBuffer;
        uint32_t boneOffset;
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
        // Only the skinned vertex shader declares this one, which is legal: a shader may use less of
        // a push constant range than the layout provides. It is the last four bytes available.
        uint32_t boneOffset;
    };

    static_assert(sizeof(GBufferPushConstants) == 128, "G-buffer push constant layout mismatch");
    static_assert(sizeof(GBufferPushConstants) <= 128, "exceeds the guaranteed maxPushConstantsSize");
    static_assert(offsetof(GBufferPushConstants, normalMatrix) == 64, "normalMatrix must be at byte 64");
    static_assert(offsetof(GBufferPushConstants, textureIndex) == 112, "textureIndex must be at byte 112");
    static_assert(offsetof(GBufferPushConstants, roughness) == 116, "roughness must be at byte 116");
    static_assert(offsetof(GBufferPushConstants, specularStrength) == 120, "specularStrength at byte 120");
    static_assert(offsetof(GBufferPushConstants, boneOffset) == 124, "boneOffset must be at byte 124");

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

    // One frame's half of the temporal accumulator's ping-pong. Two of these exist per image, and the
    // set bound on frame N reads index 1 - N and writes index N.
    struct DenoiseTarget
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    // One-bounce indirect light, written by raygen and sampled by the composite pass.
    //   .rgb = albedo of whatever the hemisphere ray hit, i.e. the colour the bounce carries
    //   .a   = sky visibility, which is also exactly the ambient occlusion term
    //
    // Those two come from a single ray. The hemisphere ray uses miss index 0, so escaping it returns
    // through miss.rmiss with w = -1, and hitting returns through closesthit.rchit with w = the hit
    // distance and rgb = the surface albedo. Occlusion is the sign of w and the bounce colour is the
    // rgb, so ambient occlusion is not a separate feature with a separate ray -- it is the shadow
    // that one-bounce GI casts.
    //
    // **The stored rgb is albedo, not radiance, and that distinction is load-bearing.** Sky and sun
    // colour move with the weather, sunrise and the lightning flash; accumulating a lit colour would
    // put all of that into a 32-frame history and smear a lightning flash across half a second. The
    // albedo of a rock is frame-invariant, so composite.frag multiplies it by the current light
    // instead. This is the same argument that made the accumulator store visibility rather than
    // radiance, applied to a colour.
    //
    // R16G16B16A16_SFLOAT because it is a mandatory storage image format. DENOISER-PLAN.md §1 wants
    // packed formats here to save bandwidth, but R16G16_SFLOAT and B10G11R11_UFLOAT_PACK32 both need
    // shaderStorageImageExtendedFormats, which this device does not enable -- so that route costs a
    // feature query plus a fallback path, for a saving that cannot be measured without a Deck.
    constexpr VkFormat rtIndirectFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // The accumulated signal. R16G16B16A16_SFLOAT rather than an 8-bit format, which looks tempting
    // for a mask in [0, 1] and is wrong: an exponential accumulator moves the stored value by
    // alpha * delta, so at 8 bits and alpha = 1/32 convergence stalls entirely whenever the change is
    // under about 0.06. That is a dead zone in the middle of every penumbra -- exactly where the soft
    // shadows this exists for live. fp16 carries ~11 bits of mantissa near 1.0 and has no such floor.
    constexpr VkFormat denoiseHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // Packed depth and normal of whatever the accumulator last stored at each pixel, for the
    // rejection tests. R32G32_UINT rather than a float format for two reasons: view-space Z is in
    // world units and Morrowind's view distances overflow fp16's precision long before its range
    // becomes the problem (fp16 resolves ~3 units at 6000, the same order as the tolerance being
    // measured), and a _UINT image cannot be linearly filtered -- which is the point, because every
    // history tap needs its own validity test and therefore its own unfiltered fetch.
    constexpr VkFormat denoiseGeomFormat = VK_FORMAT_R32G32_UINT;

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

        // Uploads this frame's point lights. Must be called before updateScene, because it is what
        // sets the light count the scene data carries. Anything past maxPointLights is dropped, with
        // a single warning rather than a crash.
        void updateLights(const PointLight* lights, uint32_t count);

        // Uploads this frame's bone palettes, as \a count column-major 4x4 matrices laid end to end.
        // A submission's boneOffset indexes this array, and its per-vertex bone indices are relative
        // to that offset.
        //
        // Returns how many matrices were actually taken. Anything past maxSkinMatrices is dropped, so
        // a caller that gets back less than it gave must not submit the shapes whose palettes fell
        // off the end as skinned -- they would read another shape's bones.
        uint32_t updateSkinMatrices(const float* matrices, uint32_t count);

        void submitMesh(const MeshSubmission& submission);

        // Rewrites the G-buffer sampler array. views[i] is placed in slot i + 1; slot 0 and any slot
        // left over stay pointed at the white fallback texture. Callers therefore submit meshes with
        // textureIndex = i + 1, or 0 for "untextured". Views beyond maxSceneTextures - 1 are dropped
        // (they keep sampling the fallback) with a single warning rather than a crash.
        void setTextures(const std::vector<VkImageView>& views);

        // Requests a TLAS rebuild on the next frame. The TLAS is only rebuilt when the instance set
        // actually changes (i.e. on cell load/unload) rather than every frame: a rebuild allocates a
        // new acceleration structure and has to idle the device to retire the old one safely.
        // Also invalidates the temporal accumulator's history. Set here rather than in buildTlas
        // because the ordering works out: callers mark the TLAS dirty during cell sync, which runs
        // before updateScene, which runs before render() actually rebuilds. So the frame that first
        // sees the new geometry is also the frame that discards the history for it.
        void markTlasDirty()
        {
            mTlasDirty = true;
            mHistoryInvalid = true;
        }

        // Exposed so callers can build GPU resources (e.g. NifVk::MeshConverter) against this device.
        Device& device() { return *mDevice; }
        CommandPool& commandPool() { return *mCommandPool; }

        // Records 2D overlay draws -- the user interface -- into the composite render pass, after the
        // fullscreen composite draw and before the pass ends.
        //
        // A callback rather than a member subsystem because the interface has to stay downstream of
        // this component: MyGUI lives in the dependency bundle and components/vk deliberately does not
        // know about it, any more than it knows about OSG. It is nevertheless part of the renderer in
        // the sense that matters -- Quake II put Draw_Pic inside the refresh interface precisely so
        // that whichever backend the player selected drew the HUD too, and nothing composited one
        // renderer's output into another's window.
        using OverlayCallback = std::function<void(VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent)>;
        void setOverlayCallback(OverlayCallback callback) { mOverlayCallback = std::move(callback); }

        // The render pass an overlay pipeline must be created against. Fixed for the renderer's
        // lifetime -- a resize recreates the framebuffers but not the pass -- so an overlay can build
        // its pipelines once at startup and never revisit them.
        VkRenderPass compositeRenderPass() const { return mCompositeRenderPass; }

        // Current swapchain extent, so an overlay can size itself without holding a Swapchain.
        VkExtent2D swapchainExtent() const;

        // Blocks until the device is idle. Callers that own GPU resources referenced by submitted
        // command buffers -- cell geometry, terrain -- must call this before destroying them, because
        // up to maxFramesInFlight submissions may still be reading those buffers and BLASes.
        void waitIdle();

        // Asks for a copy of the next frame. The copy is recorded into that frame's own command
        // buffer, so this must be called before the render() that should be captured; takeScreenshot
        // collects the result afterwards.
        //
        // Two phases rather than one call, and not for tidiness. A presented swapchain image belongs
        // to the presentation engine: touching it without acquiring it again is illegal, and
        // vkDeviceWaitIdle does not synchronise with presentation. Doing it the obvious way -- wait
        // for idle, then copy out of the last presented image -- produces exactly two validation
        // errors, WRITE_AFTER_PRESENT and "layout transition on presentable image that has not been
        // acquired", and it was written that way first. The image is only ours between acquire and
        // present, so the copy has to happen inside the frame.
        //
        // This exists because screen capture is not a reliable way to see what this renderer drew.
        // A window whose swapchain the compositor has put on a hardware overlay plane reads back as
        // solid black through BitBlt and through PrintWindow alike -- the capture succeeds and the
        // pixels are simply not the window's. A whole debugging session went on a renderer that was
        // working perfectly and only looked dead. Reading the image out of the swapchain does not go
        // past the compositor at all, so it cannot be lied to in that way.
        void requestScreenshot() { mScreenshotRequested = true; }

        // Hands over the frame requested above, eight bits per channel, row major, top row first,
        // alpha forced opaque, and clears it. False if no screenshot has been captured since the last
        // call. Meant for a screenshot key: the capturing frame blocks on the device once.
        bool takeScreenshot(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);

    private:
        // Records the copy out of the swapchain image into mScreenshotBuffer, inside the frame's
        // command buffer while the image is still ours.
        void recordScreenshotCopy(VkCommandBuffer cmd, VkExtent2D extent);

        // Reads mScreenshotBuffer back once the submission that filled it has completed.
        void resolveScreenshot();

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
        void createDenoiseTargets();
        void destroyDenoiseTargets();
        void createRtDescriptorSets();
        void buildTlas();
        // Uploads the per-instance GeometryRecord array the hit shaders index with
        // gl_InstanceCustomIndexEXT, growing the device buffer when the instance count does. Called
        // from buildTlas so the table and the TLAS instance order can never disagree.
        void uploadGeometryTable(const std::vector<GeometryRecord>& records);
        void writeCompositeDescriptor(uint32_t binding, VkImageView view);
        // Binding 8, the denoise history, which composite reads for the per-pixel history length its
        // spatial fallback filter is gated on. Separate because it is the one composite binding whose
        // view differs per frame in flight.
        void writeCompositeHistoryDescriptors();

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
        // One-bounce indirect light and ambient occlusion. Created, destroyed, cleared and
        // transitioned in lockstep with mRtOutput, so anything done to one must be done to the other.
        RtOutputImage mRtIndirect;

        // Temporal accumulator history. Read and write are separate images rather than one
        // read-modify-write target because they have to be: pixel A reads the history at pixel B's
        // reprojected location, which pixel B may already have overwritten. There is no ordering
        // between raygen invocations, so a single buffer is a data race with no way to fix it.
        std::array<DenoiseTarget, maxFramesInFlight> mDenoiseHistory = {};
        std::array<DenoiseTarget, maxFramesInFlight> mDenoiseGeom = {};
        // Accumulated bounce albedo, ping-ponged exactly like mDenoiseHistory and sharing its
        // reprojection, rejection tests and blend weight. A separate pair rather than more channels
        // on mDenoiseHistory because that image is full: .r visibility, .g its second moment, .b sky
        // visibility, .a history length.
        std::array<DenoiseTarget, maxFramesInFlight> mDenoiseIndirect = {};
        // Set by buildTlas, consumed by the next updateScene. A cell load changes what casts shadows,
        // and the depth and normal rejection tests cannot see that: a newly loaded building throwing a
        // new shadow across unchanged ground passes both tests and would keep its stale history.
        bool mHistoryInvalid = false;

        VkRenderPass mGBufferRenderPass = VK_NULL_HANDLE;
        VkRenderPass mCompositeRenderPass = VK_NULL_HANDLE;

        VkFramebuffer mGBufferFramebuffer = VK_NULL_HANDLE;
        std::vector<VkFramebuffer> mCompositeFramebuffers;

        VkPipeline mGBufferPipeline = VK_NULL_HANDLE;
        VkPipelineLayout mGBufferPipelineLayout = VK_NULL_HANDLE;
        // The same pipeline with a second vertex binding and a vertex shader that poses the vertex.
        // It shares mGBufferPipelineLayout, so switching between the two mid-pass costs a bind and
        // nothing else -- the descriptor set and the push constants carry across.
        VkPipeline mGBufferSkinnedPipeline = VK_NULL_HANDLE;
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

        // One point light buffer per frame in flight, persistently mapped. Per-frame rather than
        // shared because the CPU rewrites it every frame while the previous frame's submission may
        // still be reading it.
        std::array<VkBuffer, maxFramesInFlight> mLightBuffers = {};
        std::array<VmaAllocation, maxFramesInFlight> mLightMemory = {};
        std::array<void*, maxFramesInFlight> mLightMapped = {};
        uint32_t mLightCount = 0;
        bool mLightOverflowWarned = false;

        void createLightBuffers();

        // Bone palettes, per frame in flight and persistently mapped for the same reason the light
        // buffers are: rewritten every frame while the previous frame may still be reading.
        std::array<VkBuffer, maxFramesInFlight> mSkinBuffers = {};
        std::array<VmaAllocation, maxFramesInFlight> mSkinMemory = {};
        std::array<void*, maxFramesInFlight> mSkinMapped = {};
        bool mSkinOverflowWarned = false;

        void createSkinBuffers();

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

        OverlayCallback mOverlayCallback;

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

        // Screenshot state. mScreenshotRequested is set from outside and consumed by the next
        // render(); mScreenshotPixels holds the result until someone takes it.
        bool mScreenshotRequested = false;
        bool mScreenshotPending = false;
        bool mScreenshotReady = false;
        std::unique_ptr<Buffer> mScreenshotBuffer;
        VkDeviceSize mScreenshotBufferSize = 0;
        std::vector<uint8_t> mScreenshotPixels;
        uint32_t mScreenshotWidth = 0;
        uint32_t mScreenshotHeight = 0;
        bool mRayTracingEnabled = false;
        bool mTlasDirty = false;
    };
}

#endif
