#version 460

// Expands one sky billboard -- the sun disc or one of the two moons -- into two triangles. The corner
// comes from gl_VertexIndex and everything else from the push constant block, so there is no vertex
// buffer, no index buffer and no storage buffer.
//
// Push constants rather than the storage buffer particle.vert reads, because there are exactly three
// of these in the whole game. A buffer would cost a descriptor binding, a mapped allocation per frame
// in flight and a write every frame, to save two vkCmdDraw calls.

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

// 112 bytes, inside the 128 Vulkan guarantees everywhere. Both stages declare it identically because
// a push constant block is one range shared by the whole pipeline, and Vk::SkyElement in
// components/vk/vkrenderer.hpp is the same layout a third time with static_asserts pinning it.
layout(push_constant) uniform SkyPush {
    vec4 position;       // world-space centre of the quad
    vec4 right;          // world-space half extent along the quad's local +X, which is also +u
    vec4 up;             // world-space half extent along the quad's local +Y, which is also +v
    vec4 colour;         // .rgb tint, .a fade
    vec4 moonBlend;      // moon only
    vec4 atmosphereFade; // moon only
    uvec4 params;        // .x = diffuse slot, .y = mask slot, .z != 0 for a moon
} sky;

// How much of the sun disc is unoccluded, measured by raygen.rgen earlier in this frame. Both
// stages read it: the flash reads it here to decide how big to be and again in sky.frag to
// decide how bright.
//
// Mirrors Vk::SunVisibility. The layout is pinned by static_asserts on the C++ side and by
// nothing whatsoever here or in sky.frag or raygen.rgen -- four declarations of one block.
layout(set = 1, binding = 10) readonly buffer SunVisibilityBuffer {
    vec4 discDir;
    uvec4 params;
    uint totalRays;
    uint visibleRays;
    uint sunVisPad0;
    uint sunVisPad1;
} sunVis;

// What OcclusionCallback::getVisibleRatio answers upstream (skyutil.cpp lines 136-159), minus
// the rate limiter.
//
// Upstream clamps this to a change of ten per cent of its range per second. That filter is not
// smoothing a noisy signal -- it is hiding the latency of a GL occlusion query, whose result
// arrives at least a frame after the geometry that produced it and can arrive several frames
// later. The cost of it is that the flash takes a full second to die when the sun goes behind a
// wall, and the same second to come back. There is nothing to hide here: the count was taken
// this frame from a stratified sample set, so it is already stable, and copying the rate limit
// would be copying a workaround for a problem this renderer does not have.
float visibleRatio() {
    // No rays means the disc was never sampled: ray tracing is off, or the caller set the count
    // to zero. One is the right answer for that, not zero -- a build with no ray tracing then
    // draws the flash and the glare at the strength the CPU already worked out, rather than
    // deleting both effects and leaving no sign of why.
    if (sunVis.totalRays == 0u)
        return 1.0;

    return float(sunVis.visibleRays) / float(sunVis.totalRays);
}

layout(location = 0) out vec2 fragUv;

void main() {
    // Two triangles, six vertices, corners in the order (0,0) (1,0) (1,1) (0,0) (1,1) (0,1) -- the
    // same order particle.vert uses.
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );
    vec2 corner = corners[gl_VertexIndex];

    // Straight through as the texture coordinate, which reproduces the pairing skyutil.cpp's
    // createTexturedQuad authors: its vertex at local (-0.5, -0.5) carries uv (0, 0). Any global flip
    // between the two backends would already be wrong for every mesh and every particle in this
    // renderer, so this deliberately adds none of its own.
    fragUv = corner;

    // NOT billboarded against the camera, which is the one place this does not copy particle.vert.
    // The right and up axes arrive already rotated by the PositionAttitudeTransform OSG orients the
    // body with, so the quad ends up with exactly the orientation OSG gives it -- including the roll
    // about the view direction, which is what points a crescent moon's horns. Rebuilding the axes from
    // the view matrix would face the quad correctly and spin the phase image as the player turned,
    // which reads as the moon rotating in place.
    // The glare is not a billboard and does not want the camera at all. Sun::createSunGlare
    // hangs its quad off a nested osg::Camera with an identity view and an identity projection
    // (skyutil.cpp lines 838-851), which means the quad's own coordinates -- (-1,-1) to (1,1)
    // -- are already clip space. Emitting them straight through is that same camera, expressed
    // as the two lines it actually amounts to.
    if (sky.params.z == 3u) {
        gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
        return;
    }

    vec2 offset = (corner - 0.5) * 2.0;

    // The sun flash grows and shrinks with how much of the sun is showing, and this is the one
    // thing in this renderer's sky that is recomputed rather than read. It has to be:
    // SunFlashCallback applies the scale by pushing a scaled model-view matrix at cull time
    // (skyutil.cpp lines 210-217), and cull-time state is invisible to the node visitor the
    // reader walks the graph with. The graph always holds the unscaled 2.6 quad.
    //
    // Line for line from that callback, including the discontinuity at a tenth. Below it the
    // quad is drawn at the raw ratio; above it the ratio is remapped into 0.6 to 1 so the halo
    // never collapses to a speck while the sun is plainly visible. The jump from 0.099 to 0.64
    // is upstream's and is kept: it is invisible because sky.frag's alpha ramp has only just
    // reached full strength at the same point, so the small quad it applies to is nearly
    // transparent. Smooth one without the other and the seam appears.
    if (sky.params.z == 2u) {
        float ratio = visibleRatio();
        float scale = ratio < 0.1 ? ratio : (ratio < 1.0 ? ratio * 0.4 + 0.6 : 1.0);
        offset *= scale;
    }

    vec3 world = sky.position.xyz + sky.right.xyz * offset.x + sky.up.xyz * offset.y;

    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
}
