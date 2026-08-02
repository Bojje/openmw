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

    /// Builds the terrain geometry for one exterior cell, split into one chunk per distinct land
    /// texture. Vertex positions are cell-local: X/Y in [0, 8192] measured from the cell's south-west
    /// corner, Z the absolute world height. Returns empty if the cell has no LAND record or no
    /// height data.
    std::vector<LandChunk> buildLandChunks(int cellX, int cellY);
}

#endif
#endif
