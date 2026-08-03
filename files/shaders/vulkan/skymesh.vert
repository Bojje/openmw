#version 460

// The two sky meshes that are real geometry rather than a billboard: the cloud layer and the night sky
// dome. A port of files/shaders/compatibility/sky.vert for the PASS_CLOUDS and PASS_ATMOSPHERE_NIGHT
// branches, which between them are two lines of it.
//
// This one has a vertex buffer, unlike sky.vert next to it, and that is the whole reason it exists.
// The sun and the moons are quads whose four corners can be built from gl_VertexIndex; these are NIF
// meshes with per-vertex alpha that OpenMW writes after loading them, and no amount of arithmetic in a
// shader recovers that. MWRender::SkyMeshCache uploads them once.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

// Declared exactly as skymesh.frag declares it, down to the name, even though this stage only reads
// the first two matrices. Two stages of one pipeline that describe the same binding differently is a
// thing the spec leaves less room for than it looks, and it is not worth finding out on a Deck: the
// unused members cost nothing at all, since the block is a view onto a buffer that already exists.
// Vk::SceneData is the authority on the layout and its static_asserts pin it.
layout(set = 0, binding = 0) uniform SceneUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 skyColor;
    vec4 fogColor;
} scene;

// 112 bytes, inside the 128 Vulkan guarantees everywhere. Both stages declare it identically because a
// push constant block is one range shared by the whole pipeline, and Vk::SkyMeshPush in
// components/vk/vkrenderer.hpp is the same layout a third time with static_asserts pinning it.
layout(push_constant) uniform SkyMeshPush {
    mat4 model;    // mesh to world, with the sky's camera-relative offset already added back in
    vec4 emission; // .rgb the material's emission colour decoded to linear, .a the opacity uniform
    vec4 uvOffset; // .xy the translation on the mesh's texture matrix, .zw unused
    uvec4 params;  // .x = sampler slot, .y = the sky pass, .zw unused
} mesh;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out float fragAlpha;

void main() {
    // The scroll, and it is added here rather than recomputed. OpenMW advances one float per frame --
    // SkyManager::update, sky.cpp lines 548-558 -- and hands it to the mesh as an osg::TexMat translate
    // on Y, which sky.vert applies as gl_TextureMatrix[0] * gl_MultiTexCoord0. The reader lifts the
    // translation straight off that matrix, so the clouds here drift at exactly the rate they drift at
    // in the other backend, including the Weather_Timescale_Clouds branch that ties them to the game
    // clock. The night sky has no texture matrix and arrives with a zero offset, which is why this
    // needs no branch on the pass.
    fragUv = inTexCoord + mesh.uvOffset.xy;

    // The alpha ModVertexAlphaVisitor wrote. This is the entire horizon fade: 0 on the cloud mesh's
    // bottom row, a quarter on the row above it, 1 everywhere else. Dropping it does not look like a
    // missing feature, it looks like the cloud layer has a hard edge cut across the sky.
    fragAlpha = inColor.a;

    gl_Position = scene.projection * scene.view * mesh.model * vec4(inPosition, 1.0);
}
