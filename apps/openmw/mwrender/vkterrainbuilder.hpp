#ifndef OPENMW_MWRENDER_VKTERRAINBUILDER_H
#define OPENMW_MWRENDER_VKTERRAINBUILDER_H

#ifdef OPENMW_USE_VULKAN

#include <cstdint>
#include <string>
#include <vector>

namespace MWRender
{
    /// One draw's worth of terrain: the quads of a cell that share a single land texture.
    struct LandChunk
    {
        /// Land texture path as stored in the LTEX record, e.g. "tx_bc_rock_01.tga". Not yet run
        /// through Misc::ResourceHelpers::correctTexturePath -- the caller's texture loader does that.
        std::string texture;
        /// Interleaved, 12 floats (48 bytes) per vertex, matching the G-buffer vertex layout:
        /// position vec3, normal vec3, texcoord vec2, colour vec4.
        std::vector<float> vertices;
        std::vector<std::uint32_t> indices;
    };

    /// Blend map resolution for one cell. 17 samples for 16 texels -- the grid is vertex-centred, so
    /// there is one extra sample, borrowed from a neighbouring cell. Doubled on upload because the
    /// quarter-texel sampling nudge is exactly half a doubled pixel and cannot be expressed without
    /// it; upstream doubles it for the same reason and calls it "to look like vanilla".
    inline constexpr int sBlendSamples = 17;
    inline constexpr int sBlendImageSize = sBlendSamples * 2;

    /// The land textures covering one cell and, for each, an alpha map saying where it wins.
    ///
    /// The weights sum to exactly 1 at every point, so the consumer composites as a weighted sum --
    /// `sum(layer_i * alpha_i)` -- and not as alpha-over. Getting that wrong is invisible where two
    /// layers meet and wrong wherever three or more do.
    struct LandBlend
    {
        /// Land texture paths, in layer order. One entry means the cell is a single texture.
        std::vector<std::string> layers;
        /// One sBlendImageSize squared alpha map per layer, row-major, rows increasing with world Y.
        /// **Empty when `layers` has one entry**, because a single texture needs no blending and the
        /// caller should skip the bake and use that texture directly.
        std::vector<std::vector<std::uint8_t>> maps;
    };

    /// Builds the terrain geometry for one exterior cell, split into one chunk per distinct land
    /// texture. Vertex positions are cell-local: X/Y in [0, 8192] measured from the cell's south-west
    /// corner, Z the absolute world height. Returns empty if the cell has no LAND record or no
    /// height data.
    ///
    /// With \a oneChunk the split is skipped entirely: one chunk covers the cell, its `texture` is
    /// left empty because no single land texture describes it, and its UVs run 0..1 rather than 0..16
    /// so that a baked composite maps across the cell exactly once. That is the path that gets blended
    /// ground; the split path is the fallback for a cell with a single texture, or one whose composite
    /// could not be baked.
    std::vector<LandChunk> buildLandChunks(int cellX, int cellY, bool oneChunk = false);

    /// The blend maps for one exterior cell. Empty `layers` if the cell has no LAND record.
    ///
    /// One arithmetic check, because every plausible mistake here produces a plausible-looking map.
    /// The sampling UV is blendU = (16u + 0.75)/17 and blendV = (16v + 0.25)/17, so the exact centre
    /// of a cell lands on sample (8, 8), which reads VTEX (x = 7, y = 8) -- mTextures[135], not
    /// mTextures[136]. Verified on cell (-5, -4) of Morrowind.esm on 2026-08-03: the centre resolves
    /// to tx_bc_moss.tga where mTextures[136] is tx_bc_grass.tga, so the check discriminates rather
    /// than merely passing. Losing the quarter-texel nudge, or borrowing the extra sample from the
    /// wrong side, moves the answer to the second of those.
    LandBlend buildLandBlend(int cellX, int cellY);
}

#endif
#endif
