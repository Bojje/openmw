#version 460

// One layer of a cell's land texture composite.
//
// The bake draws this once per layer into a single target with additive blending, so the target ends
// up holding sum(layerColour * layerWeight) across every layer. That is a weighted *sum*, not an
// alpha-over composite, and it is only correct because the weights sum to exactly 1 at every point --
// each blend map sample writes full weight into exactly one layer and zero everywhere else. Compositing
// these alpha-over instead is invisible where two layers meet and wrong wherever three or more do.

layout(location = 0) in vec2 fragUv;

// The land texture. Sampled with a repeating sampler, because the diffuse tiles many times per cell.
layout(set = 0, binding = 0) uniform sampler2D layerTexture;
// This layer's weights, one 34x34 alpha map for the whole cell. Sampled with a clamping sampler: the
// UV below reaches exactly the outermost texel centres, so any floating point overshoot at the cell
// edge must clamp rather than wrap onto a texel belonging to a different layer.
layout(set = 0, binding = 1) uniform sampler2D blendMap;

layout(location = 0) out vec4 outColor;

// Land texture tiles per cell side. Morrowind paints texture on a 16x16 grid and the diffuse repeats
// once per tile, which is what makes the ground read at walking distance.
const float sTilesPerCell = 16.0;
// Blend map samples per side. One more than the tiles, because the sample grid is vertex-centred:
// there is a sample at every tile *boundary*, and the extra one is borrowed from the next cell.
const float sBlendSamples = 17.0;

void main() {
    vec3 layerColour = texture(layerTexture, fragUv * sTilesPerCell).rgb;

    // The blend UV, composed from upstream's texture matrix (Terrain::BlendmapTexMat). It shrinks by
    // tiles/(tiles+1) to fit the 17-sample grid into the cell, and nudges by a quarter texel -- east
    // on U and south on V, which is not a uniform diagonal offset and is not a rounding artifact.
    // Upstream calls it "nudge the blendmap to look like vanilla".
    //
    // The two terms cancel into this closed form: blendU = (16u + 0.75)/17, blendV = (16v + 0.25)/17.
    // Check it at the centre of a cell -- u = v = 0.5 gives sample (8, 8), which reads VTEX (7, 8) and
    // not (8, 8). Getting the nudge or its sign wrong moves the whole map by half a texel and still
    // looks entirely plausible.
    vec2 blendUv = (fragUv * sTilesPerCell + vec2(0.75, 0.25)) / sBlendSamples;

    // Deliberately not multiplied here. The pipeline blends with SRC_ALPHA, ONE, so the weight is
    // applied by the blend unit and the sum accumulates in the framebuffer.
    outColor = vec4(layerColour, texture(blendMap, blendUv).r);
}
