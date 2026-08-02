#include "vkmyguirendermanager.hpp"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <MyGUI_Timer.h>
#include <MyGUI_VertexData.h>

#include <components/debug/debuglog.hpp>
#include <components/vk/vkcommands.hpp>
#include <components/vk/vkdevice.hpp>
#include <components/vk/vkrenderer.hpp>
#include <components/vk/vkshader.hpp>

#include "vkmyguitexture.hpp"

namespace VkMyGUIPlatform
{
    namespace
    {
        // How many descriptor sets a pool holds. OpenMW's interface resolves to on the order of a
        // hundred textures -- skins, font atlases, item icons, the manually created 8x8 fills -- so
        // one pool covers a normal session and the growth path exists for mods that add more.
        constexpr uint32_t sSetsPerPool = 256;

        // Smallest vertex buffer allocated, in vertices. MyGUI asks for a buffer per render item and
        // most are a single quad, so allocating exactly what was asked for would mean reallocating
        // the moment a widget gains a second sub-skin.
        constexpr size_t sMinVertexCapacity = 64;

        // The vertex attribute descriptions below are written in terms of these offsets, and nothing
        // else would notice if MyGUI reordered its struct: an attribute reading the wrong bytes is
        // still a legal pipeline, so the failure is silently displaced geometry or colours rather
        // than a validation message. Pin what the shader was written against.
        static_assert(sizeof(MyGUI::Vertex) == 24, "MyGUI::Vertex layout drifted");
        static_assert(offsetof(MyGUI::Vertex, x) == 0, "MyGUI::Vertex position must be first");
        static_assert(offsetof(MyGUI::Vertex, colour) == 12, "MyGUI::Vertex colour must follow the position");
        static_assert(offsetof(MyGUI::Vertex, u) == 16, "MyGUI::Vertex texture coordinate must be last");
    }

    /// MyGUI::IVertexBuffer over host-visible memory the GPU reads directly.
    ///
    /// No staging copy and no device-local buffer. The interface hands out a pointer for MyGUI to
    /// write vertices into and takes it back on unlock, which is exactly the shape of a persistently
    /// mapped upload buffer, and an interface's worth of geometry is a few thousand vertices -- far
    /// below the point where the read bandwidth of host memory is worth a copy to avoid.
    ///
    /// The slot alternation is inherited from the OSG platform and its reasoning carries over intact.
    /// A buffer is only advanced when the current slot has actually been submitted, so a static
    /// widget that never relocks keeps reusing one allocation, while a buffer rewritten every frame
    /// alternates. Two slots is exactly enough for two frames in flight: the lock that returns to
    /// slot k happens two frames after the submit that read it, and beginFrame has waited on that
    /// frame's fence by then.
    class VertexBuffer final : public MyGUI::IVertexBuffer
    {
    public:
        explicit VertexBuffer(RenderManager& manager)
            : mManager(manager)
        {
        }

        ~VertexBuffer() override
        {
            for (uint32_t slot = 0; slot < Vk::maxFramesInFlight; ++slot)
                release(slot);
        }

        void setVertexCount(size_t count) override { mNeedVertexCount = count; }
        size_t getVertexCount() const override { return mNeedVertexCount; }

        MyGUI::Vertex* lock() override
        {
            if (mUsed)
            {
                mCurrent = (mCurrent + 1) % Vk::maxFramesInFlight;
                mUsed = false;
            }

            const size_t needed = std::max(mNeedVertexCount, sMinVertexCapacity);
            if (!mBuffers[mCurrent] || mCapacity[mCurrent] < needed)
                allocate(needed);

            return static_cast<MyGUI::Vertex*>(mMapped[mCurrent]);
        }

        // Nothing to do: the allocation is HOST_COHERENT, so the write is visible to the device
        // without a flush, and the submit that reads it happens later in the same frame.
        void unlock() override {}

        void markUsed() { mUsed = true; }

        VkBuffer handle() const
        {
            return mBuffers[mCurrent] ? mBuffers[mCurrent]->handle() : VK_NULL_HANDLE;
        }

    private:
        void allocate(size_t vertices)
        {
            // Grown geometrically so a widget that gains a sub-skin every few frames does not
            // reallocate every few frames.
            const size_t capacity = std::max(vertices, mCapacity[mCurrent] * 2);
            const VkDeviceSize bytes = static_cast<VkDeviceSize>(capacity) * sizeof(MyGUI::Vertex);

            auto buffer = std::make_unique<Vk::Buffer>(mManager.renderer().device(), bytes,
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            void* mapped = buffer->map();

            release(mCurrent);

            mBuffers[mCurrent] = std::move(buffer);
            mMapped[mCurrent] = mapped;
            mCapacity[mCurrent] = capacity;
        }

        void release(uint32_t slot)
        {
            if (!mBuffers[slot])
                return;

            // Unmapped before it is handed over, because VMA tracks a map count per allocation and
            // asserts on a mapped allocation being freed. The mapping is host state only, so dropping
            // it does not affect a submission still reading the buffer.
            mBuffers[slot]->unmap();
            mMapped[slot] = nullptr;
            mCapacity[slot] = 0;
            mManager.retireBuffer(std::move(mBuffers[slot]));
        }

        RenderManager& mManager;

        std::array<std::unique_ptr<Vk::Buffer>, Vk::maxFramesInFlight> mBuffers;
        std::array<void*, Vk::maxFramesInFlight> mMapped = {};
        std::array<size_t, Vk::maxFramesInFlight> mCapacity = {};

        size_t mNeedVertexCount = 0;
        uint32_t mCurrent = 0;
        bool mUsed = false;
    };

    // ---------------------------------------------------------------------------

    RenderManager::RenderManager(Vk::Renderer& renderer, Resource::ImageManager* imageManager, float scalingFactor)
        : mRenderer(renderer)
        , mImageManager(imageManager)
        , mInvScalingFactor(1.f)
    {
        if (scalingFactor != 0.f)
            mInvScalingFactor = 1.f / scalingFactor;
    }

    RenderManager::~RenderManager()
    {
        shutdown();
    }

    void RenderManager::initialise()
    {
        if (mIsInitialise)
            throw std::runtime_error("VkMyGUIPlatform::RenderManager initialised twice");

        // ColourABGR, not ColourARGB. MyGUI packs the colour into a uint32 and the two orderings put
        // red in opposite bytes; ABGR is the one whose little-endian byte order is r, g, b, a, which
        // is what a VK_FORMAT_R8G8B8A8_UNORM vertex attribute reads. Choosing ARGB here would swap
        // red and blue across the entire interface.
        mVertexFormat = MyGUI::VertexColourType::ColourABGR;
        mUpdate = false;

        createDescriptorLayout();
        createSampler();
        createFallbackTexture();

        mRenderer.setOverlayCallback([this](VkCommandBuffer cmd, uint32_t frameIndex, VkExtent2D extent) {
            recordFrame(cmd, frameIndex, extent);
        });

        mExtent = mRenderer.swapchainExtent();
        setViewSize(static_cast<int>(mExtent.width), static_cast<int>(mExtent.height));

        mIsInitialise = true;
        Log(Debug::Info) << "Vulkan MyGUI render manager initialised";
    }

    void RenderManager::shutdown()
    {
        if (!mIsInitialise)
            return;

        // Nothing below is safe while a submitted command buffer may still be reading it, and there
        // is no cheaper synchronisation available at teardown than draining the device.
        mRenderer.setOverlayCallback(nullptr);
        mRenderer.waitIdle();

        mTextures.clear();
        collectRetired(true);
        destroyPipelines();

        VkDevice device = mRenderer.device().handle();

        if (mPipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device, mPipelineLayout, nullptr);
            mPipelineLayout = VK_NULL_HANDLE;
        }

        mFallbackTexture.reset();

        if (mSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, mSampler, nullptr);
            mSampler = VK_NULL_HANDLE;
        }

        // Destroying a pool frees every set allocated from it, which is why sets are never freed
        // individually anywhere else in this file.
        for (VkDescriptorPool pool : mDescriptorPools)
            vkDestroyDescriptorPool(device, pool, nullptr);
        mDescriptorPools.clear();
        mFreeSets.clear();
        mFallbackSet = VK_NULL_HANDLE;

        if (mDescriptorLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, mDescriptorLayout, nullptr);
            mDescriptorLayout = VK_NULL_HANDLE;
        }

        mIsInitialise = false;
    }

    void RenderManager::createDescriptorLayout()
    {
        VkDescriptorSetLayoutBinding binding = {};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo = {};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = 1;
        layoutInfo.pBindings = &binding;

        VK_CHECK(vkCreateDescriptorSetLayout(
            mRenderer.device().handle(), &layoutInfo, nullptr, &mDescriptorLayout));
    }

    void RenderManager::createSampler()
    {
        VkSamplerCreateInfo samplerInfo = {};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        // No mip chain is uploaded for interface textures and none is wanted: a widget is drawn at
        // or near 1:1, and a font atlas sampled from a lower level bleeds neighbouring glyphs into
        // each other. The OSG platform sets MIN_FILTER to LINEAR for the same reason.
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.maxLod = 0.0f;
        // Clamped rather than repeating, unlike scene textures. Interface UVs address a sub-rectangle
        // of an atlas, so a coordinate that strays outside it must stop at the edge instead of
        // wrapping around and picking up an unrelated glyph.
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        VK_CHECK(vkCreateSampler(mRenderer.device().handle(), &samplerInfo, nullptr, &mSampler));
    }

    void RenderManager::createFallbackTexture()
    {
        const std::array<unsigned char, 4> white = { 255, 255, 255, 255 };
        mFallbackTexture = std::make_unique<Vk::Texture>(Vk::Texture::create(mRenderer.device(),
            mRenderer.commandPool(), 1, 1, VK_FORMAT_R8G8B8A8_SRGB, white.data(), white.size()));
        mFallbackSet = acquireTextureSet(mFallbackTexture->view());
    }

    VkDescriptorSet RenderManager::allocateSet()
    {
        if (!mFreeSets.empty())
        {
            VkDescriptorSet set = mFreeSets.back();
            mFreeSets.pop_back();
            return set;
        }

        VkDevice device = mRenderer.device().handle();

        // Two attempts: allocate from the newest pool, and if it is full or fragmented create another
        // and try once more. A second failure is not a capacity problem and is left to throw.
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            if (!mDescriptorPools.empty())
            {
                VkDescriptorSetAllocateInfo allocInfo = {};
                allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
                allocInfo.descriptorPool = mDescriptorPools.back();
                allocInfo.descriptorSetCount = 1;
                allocInfo.pSetLayouts = &mDescriptorLayout;

                VkDescriptorSet set = VK_NULL_HANDLE;
                const VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &set);
                if (result == VK_SUCCESS)
                    return set;

                // Anything other than a full or fragmented pool is a real failure and must not be
                // retried by adding memory the driver did not ask for.
                if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
                    VK_CHECK(result);
            }

            VkDescriptorPoolSize poolSize = {};
            poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            poolSize.descriptorCount = sSetsPerPool;

            VkDescriptorPoolCreateInfo poolInfo = {};
            poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            poolInfo.maxSets = sSetsPerPool;
            poolInfo.poolSizeCount = 1;
            poolInfo.pPoolSizes = &poolSize;

            VkDescriptorPool pool = VK_NULL_HANDLE;
            VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool));
            mDescriptorPools.push_back(pool);
        }

        throw std::runtime_error("Vulkan MyGUI: could not allocate a texture descriptor set");
    }

    VkDescriptorSet RenderManager::acquireTextureSet(VkImageView view)
    {
        VkDescriptorSet set = allocateSet();

        VkDescriptorImageInfo imageInfo = {};
        imageInfo.sampler = mSampler;
        imageInfo.imageView = view;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write = {};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(mRenderer.device().handle(), 1, &write, 0, nullptr);
        return set;
    }

    void RenderManager::retireTexture(std::unique_ptr<Vk::Texture> texture, VkDescriptorSet set)
    {
        if (!texture && set == VK_NULL_HANDLE)
            return;

        // Past shutdown the device has already been drained and the descriptor pools are gone, so
        // queueing would defer the free to a member destructor that may run after the device does.
        if (!mIsInitialise)
        {
            texture.reset();
            return;
        }

        mRetired.push_back({ mFrameCounter, std::move(texture), nullptr, set });
    }

    void RenderManager::retireBuffer(std::unique_ptr<Vk::Buffer> buffer)
    {
        if (!buffer)
            return;

        if (!mIsInitialise)
        {
            buffer.reset();
            return;
        }

        mRetired.push_back({ mFrameCounter, nullptr, std::move(buffer), VK_NULL_HANDLE });
    }

    void RenderManager::collectRetired(bool force)
    {
        // A resource released while frame N was being recorded may be read by that frame's submission
        // and by every frame still in flight behind it. Waiting one more than maxFramesInFlight is
        // one frame of slack past what is strictly needed, which costs one frame of memory and
        // removes any dependence on exactly when in the frame the release happened.
        const uint64_t safeBefore = mFrameCounter > Vk::maxFramesInFlight
            ? mFrameCounter - Vk::maxFramesInFlight
            : 0;

        auto expired = [&](const Retired& item) { return force || item.frame < safeBefore; };

        for (auto& item : mRetired)
        {
            if (!expired(item))
                continue;
            item.texture.reset();
            item.buffer.reset();
            if (item.set != VK_NULL_HANDLE)
                mFreeSets.push_back(item.set);
        }

        mRetired.erase(
            std::remove_if(mRetired.begin(), mRetired.end(), expired), mRetired.end());
    }

    bool RenderManager::loadShaders(const std::string& shaderDir)
    {
        auto loadShader = [&](const std::string& name) -> std::unique_ptr<Vk::ShaderModule> {
            const std::string path = shaderDir + "/" + name;
            std::ifstream test(path, std::ios::binary);
            if (!test.good())
                return nullptr;
            test.close();
            return std::make_unique<Vk::ShaderModule>(
                Vk::ShaderModule::fromFile(mRenderer.device(), path));
        };

        auto vert = loadShader("gui.vert.spv");
        auto frag = loadShader("gui.frag.spv");
        if (!vert || !frag)
        {
            Log(Debug::Warning) << "Vulkan MyGUI shaders not found in " << shaderDir
                                << ", the interface will not be drawn";
            return false;
        }

        VkDevice device = mRenderer.device().handle();

        // Only matters on a second call, which is not something that happens today. Destroying a
        // pipeline a submitted command buffer still references is undefined behaviour, and draining
        // the device is the whole cost of making a rebuild safe.
        if (mPipelines[0] != VK_NULL_HANDLE)
            mRenderer.waitIdle();

        if (mPipelineLayout == VK_NULL_HANDLE)
        {
            VkPipelineLayoutCreateInfo layoutInfo = {};
            layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            layoutInfo.setLayoutCount = 1;
            layoutInfo.pSetLayouts = &mDescriptorLayout;
            // No push constants at all. MyGUI's vertices arrive in clip space and the shader needs no
            // transform, so there is nothing per draw that is not already in the vertex data or the
            // descriptor set. That also keeps this well clear of the 128-byte guaranteed limit the
            // rest of the renderer is budgeted against.
            VK_CHECK(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &mPipelineLayout));
        }

        destroyPipelines();

        const std::array<VkPipelineShaderStageCreateInfo, 2> stages = {
            vert->stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
            frag->stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT)
        };

        // MyGUI::Vertex is float x, y, z / uint32 colour / float u, v -- 24 bytes, with the colour
        // between the position and the texture coordinate.
        VkVertexInputBindingDescription binding = {};
        binding.binding = 0;
        binding.stride = sizeof(MyGUI::Vertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 3> attributes = {};
        attributes[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(MyGUI::Vertex, x) };
        attributes[1] = { 1, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(MyGUI::Vertex, colour) };
        attributes[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(MyGUI::Vertex, u) };

        VkPipelineVertexInputStateCreateInfo vertexInput = {};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState = {};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer = {};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        // MyGUI emits both windings depending on how a skin was authored and on the Y flip the vertex
        // shader applies, and a two-dimensional quad has no meaningful facing. Culling anything here
        // makes parts of the interface vanish for reasons that look like a layout bug.
        rasterizer.cullMode = VK_CULL_MODE_NONE;

        VkPipelineMultisampleStateCreateInfo multisampling = {};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil = {};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        // The composite pass has no depth attachment, and MyGUI resolves overlap by draw order.
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;

        std::array<VkDynamicState, 2> dynamicStates = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamicState = {};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        for (size_t i = 0; i < mPipelines.size(); ++i)
        {
            VkPipelineColorBlendAttachmentState blendAttachment = {};
            blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blendAttachment.blendEnable = VK_TRUE;
            blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstColorBlendFactor = i == static_cast<size_t>(BlendMode::Additive)
                ? VK_BLEND_FACTOR_ONE
                : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
            blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
            blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

            VkPipelineColorBlendStateCreateInfo colorBlending = {};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &blendAttachment;

            VkGraphicsPipelineCreateInfo pipelineInfo = {};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
            pipelineInfo.pStages = stages.data();
            pipelineInfo.pVertexInputState = &vertexInput;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = &depthStencil;
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            pipelineInfo.layout = mPipelineLayout;
            pipelineInfo.renderPass = mRenderer.compositeRenderPass();
            pipelineInfo.subpass = 0;

            VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                &mPipelines[i]));
        }

        Log(Debug::Info) << "Vulkan MyGUI pipelines created";
        return true;
    }

    void RenderManager::destroyPipelines()
    {
        // The pipeline layout deliberately survives this. loadShaders may be called again to rebuild
        // the pipelines, and it references the layout it created earlier in the same call.
        VkDevice device = mRenderer.device().handle();

        for (VkPipeline& pipeline : mPipelines)
        {
            if (pipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(device, pipeline, nullptr);
                pipeline = VK_NULL_HANDLE;
            }
        }

        mBoundPipeline = VK_NULL_HANDLE;
    }

    // Frame

    void RenderManager::recordFrame(VkCommandBuffer cmd, uint32_t /*frameIndex*/, VkExtent2D extent)
    {
        ++mFrameCounter;
        collectRetired(false);

        // The window can be resized without anything telling MyGUI, because the resize is handled
        // inside Vk::Renderer when an acquire or present reports the swapchain out of date. Noticing
        // it here rather than adding a second notification path keeps the two from disagreeing: the
        // extent used to lay the interface out is by construction the extent being rendered into.
        if (extent.width != mExtent.width || extent.height != mExtent.height)
        {
            mExtent = extent;
            setViewSize(static_cast<int>(extent.width), static_cast<int>(extent.height));
        }

        // Widget animations and controllers. The OSG platform runs this from the update traversal;
        // here the composite pass is the only per-frame hook there is, so it runs at the head of it.
        {
            static MyGUI::Timer timer;
            static unsigned long lastTime = timer.getMilliseconds();
            const unsigned long nowTime = timer.getMilliseconds();
            onFrameEvent(static_cast<float>(static_cast<double>(nowTime - lastTime) / 1000.0));
            lastTime = nowTime;
        }

        mCommandBuffer = cmd;

        begin();
        onRenderToTarget(this, mUpdate);
        end();

        mUpdate = false;
        mCommandBuffer = VK_NULL_HANDLE;
    }

    void RenderManager::begin()
    {
        if (mCommandBuffer == VK_NULL_HANDLE)
            return;

        mBlendMode = BlendMode::Alpha;
        mBoundPipeline = VK_NULL_HANDLE;

        // Set even though the composite draw has already set the same values: if the composite
        // pipeline failed to load, nothing did.
        VkViewport viewport = {};
        viewport.width = static_cast<float>(mExtent.width);
        viewport.height = static_cast<float>(mExtent.height);
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(mCommandBuffer, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.extent = mExtent;
        vkCmdSetScissor(mCommandBuffer, 0, 1, &scissor);
    }

    void RenderManager::end() {}

    void RenderManager::doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count)
    {
        if (mCommandBuffer == VK_NULL_HANDLE || buffer == nullptr || count == 0)
            return;

        VkPipeline pipeline = mPipelines[static_cast<size_t>(mBlendMode)];
        if (pipeline == VK_NULL_HANDLE)
            return;

        VertexBuffer* vertexBuffer = static_cast<VertexBuffer*>(buffer);
        VkBuffer handle = vertexBuffer->handle();
        if (handle == VK_NULL_HANDLE)
            return;
        vertexBuffer->markUsed();

        if (pipeline != mBoundPipeline)
        {
            vkCmdBindPipeline(mCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
            mBoundPipeline = pipeline;
        }

        // dynamic_cast rather than static_cast, and the cost is worth naming. MyGUI itself only ever
        // hands back textures this manager created, but OpenMW constructs platform textures directly
        // in a dozen places under apps/openmw/mwgui -- the character preview, the local and global
        // map, the video widget, the loading screen -- and those are the OSG platform's type. A
        // static_cast down to the wrong type is undefined behaviour that would present as a crash
        // deep inside a widget; this way an unrecognised texture draws as plain vertex colour, which
        // is visibly wrong in exactly the place that needs fixing.
        VkDescriptorSet set = mFallbackSet;
        if (Texture* vkTexture = dynamic_cast<Texture*>(texture))
        {
            if (vkTexture->descriptorSet() != VK_NULL_HANDLE)
                set = vkTexture->descriptorSet();
        }

        vkCmdBindDescriptorSets(mCommandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
            0, 1, &set, 0, nullptr);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(mCommandBuffer, 0, 1, &handle, &offset);
        vkCmdDraw(mCommandBuffer, static_cast<uint32_t>(count), 1, 0, 0);
    }

    void RenderManager::setViewSize(int width, int height)
    {
        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;

        mViewSize.set(static_cast<int>(width * mInvScalingFactor), static_cast<int>(height * mInvScalingFactor));

        mInfo.maximumDepth = 1;
        mInfo.hOffset = 0;
        mInfo.vOffset = 0;
        mInfo.aspectCoef = float(mViewSize.height) / float(mViewSize.width);
        mInfo.pixScaleX = 1.0f / float(mViewSize.width);
        mInfo.pixScaleY = 1.0f / float(mViewSize.height);

        onResizeView(mViewSize);
        mUpdate = true;
    }

    // Resources

    MyGUI::IVertexBuffer* RenderManager::createVertexBuffer()
    {
        return new VertexBuffer(*this);
    }

    void RenderManager::destroyVertexBuffer(MyGUI::IVertexBuffer* buffer)
    {
        delete buffer;
    }

    MyGUI::ITexture* RenderManager::createTexture(const std::string& name)
    {
        auto texture = std::make_unique<Texture>(name, *this);
        const auto it = mTextures.insert_or_assign(name, std::move(texture)).first;
        return it->second.get();
    }

    void RenderManager::destroyTexture(MyGUI::ITexture* texture)
    {
        if (texture == nullptr)
            return;

        const auto item = mTextures.find(texture->getName());
        if (item == mTextures.end())
        {
            Log(Debug::Warning) << "Vulkan MyGUI: texture '" << texture->getName() << "' not found";
            return;
        }

        mTextures.erase(item);
    }

    MyGUI::ITexture* RenderManager::getTexture(const std::string& name)
    {
        if (name.empty())
            return nullptr;

        const auto item = mTextures.find(name);
        if (item == mTextures.end())
        {
            MyGUI::ITexture* texture = createTexture(name);
            texture->loadFromFile(name);
            return texture;
        }
        return item->second.get();
    }

    bool RenderManager::isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage /*usage*/)
    {
        // Every format MyGUI knows about is accepted, because the ones Vulkan has no direct
        // equivalent for -- single channel luminance, and three-channel colour, which is optional for
        // sampled images -- are expanded to RGBA on upload rather than refused.
        return format != MyGUI::PixelFormat::Unknow;
    }

    bool RenderManager::checkTexture(MyGUI::ITexture* /*texture*/)
    {
        // Externally created textures are legal here as they are in the OSG platform, so there is
        // nothing this can usefully answer.
        return true;
    }

    void RenderManager::registerShader(const std::string& /*shaderName*/, const std::string& /*vertexProgramFile*/,
        const std::string& /*fragmentProgramFile*/)
    {
        Log(Debug::Warning) << "VkMyGUIPlatform::RenderManager::registerShader is not implemented";
    }

}
