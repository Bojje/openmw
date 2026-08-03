#ifndef OPENMW_MWRENDER_VKLANDCOMPOSITE_H
#define OPENMW_MWRENDER_VKLANDCOMPOSITE_H

#ifdef OPENMW_USE_VULKAN

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace Vk
{
    class Device;
    class CommandPool;
    class Texture;
}

namespace MWRender
{
    struct LandBlend;

    /// Bakes a cell's land textures into one composite texture, the way OSG's composite maps do.
    ///
    /// ESM3 paints terrain on a flat 16x16 grid of texture indices with no weights and no alpha, so
    /// every soft transition in OSG's terrain is manufactured from a blend map rather than read from
    /// the data. OSG builds one alpha map per layer and draws the layers over each other into an
    /// offscreen target; with `composite map level` at its default of 0 that is what it does for
    /// every chunk, so the shipping configuration is already one texture and one draw per chunk.
    /// This reproduces that on the Vulkan side.
    ///
    /// Baking rather than blending per pixel in the G-buffer, for one hard reason: GBufferPushConstants
    /// is exactly full at 128 bytes, so four layer indices and a blend map index cannot be pushed. A
    /// per-pixel path is still open -- one draw can already sample four land textures and a blend map,
    /// since all 512 sampler slots are bound for every draw -- but it needs a per-draw buffer, and a
    /// bake needs none of that and matches what OSG actually ships.
    ///
    /// It is not a CPU bake because it cannot be: land textures arrive as BC1 blocks and there is no
    /// block decoder anywhere in the tree. The one OSG offers is behind a branch that only runs when
    /// S3TC is unsupported, which cannot happen on a machine running this renderer.
    class LandComposite
    {
    public:
        /// Throws if the shaders are missing or the pipeline cannot be built. The caller should treat
        /// that as "no compositing", not as fatal -- terrain still draws, with hard tile edges.
        LandComposite(Vk::Device& device, Vk::CommandPool& commandPool, const std::string& shaderDir);
        ~LandComposite();

        LandComposite(const LandComposite&) = delete;
        LandComposite& operator=(const LandComposite&) = delete;

        /// Composites \a blend's layers into one texture, given the image views of the layer textures
        /// in the same order as `blend.layers`.
        ///
        /// Returns an invalid texture when there is nothing to composite -- fewer than two layers, a
        /// view count that does not match, or a bake that failed. A single-layer cell needs no
        /// composite at all and the caller should use that layer's texture directly.
        ///
        /// Synchronous: it submits and waits, like the geometry upload does, because it runs at cell
        /// load and off the frame loop.
        Vk::Texture bake(const LandBlend& blend, const std::vector<VkImageView>& layerViews);

        /// Side length of the baked texture. 1024 rather than OSG's 512 because this covers a whole
        /// cell where an OSG chunk covers a quarter of one, so this is the same texel density.
        static constexpr uint32_t sSize = 1024;

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
#endif
