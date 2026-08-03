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
    vec2 offset = (corner - 0.5) * 2.0;
    vec3 world = sky.position.xyz + sky.right.xyz * offset.x + sky.up.xyz * offset.y;

    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
}
