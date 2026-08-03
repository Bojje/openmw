#version 460

// The enchanted item glow, drawn as a second pass over geometry the G-buffer has already drawn.
//
// A second draw rather than a term folded into gbuffer.frag, and that is forced rather than
// chosen: Vk::GBufferPushConstants is exactly 128 bytes with every offset pinned by a
// static_assert, and its own comment says "Only 4 bytes of headroom remain. Anything further --
// emissive, a material index -- has to go in a per-instance buffer rather than here." A glow needs
// a colour and a texture slot, which is sixteen.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
// Locations 2 and 3 -- uv and vertex colour -- are declared by the vertex binding but unused here.
// The binding has to describe the whole 48-byte vertex either way, because this draws the same
// buffers the G-buffer pass does.

// Must match Vk::GlowPushConstants and the block in glow.frag byte for byte. Same shape as the
// G-buffer's, so that the mat3 padding rule reads the same way in both places:
//   model         offset   0, 64 bytes
//   normalMatrix  offset  64, 48 bytes  (three columns, each padded out to a vec4)
//   glowColour    offset 112, 12 bytes
//   textureIndex  offset 124,  4 bytes
// 128 bytes exactly, which is the maxPushConstantsSize Vulkan guarantees everywhere.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)   mat4 model;
    layout(offset = 64)  mat3 normalMatrix;
    layout(offset = 112) vec3 glowColour;
    layout(offset = 124) uint textureIndex;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

layout(location = 0) out vec2 fragEnvUv;
layout(location = 1) flat out vec3 fragColour;
layout(location = 2) flat out uint fragTexture;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);

    // The sphere-map coordinate, ported from objects.vert lines 105-110. It is what gives the glow
    // its character: the caustics slide across the surface as the camera moves rather than sitting
    // on it like a decal, because the coordinate comes from the reflection vector and not from the
    // mesh's own UVs.
    vec3 viewNormal = normalize(mat3(camera.view) * normalize(push.normalMatrix * inNormal));
    vec3 viewVec = normalize((camera.view * worldPos).xyz);
    vec3 r = reflect(viewVec, viewNormal);
    float m = 2.0 * sqrt(r.x * r.x + r.y * r.y + (r.z + 1.0) * (r.z + 1.0));
    fragEnvUv = vec2(r.x / m + 0.5, r.y / m + 0.5);

    fragColour = push.glowColour;
    fragTexture = push.textureIndex;

    // Written as `projection * view * worldPos`, in that association, because gbuffer.vert writes
    // it that way. This pass has no depth attachment and compares against the G-buffer depth by
    // hand, so the two shaders have to agree on this fragment's depth to the last bit. Computing it
    // as `projection * (view * worldPos)` here -- which is the same value in real arithmetic and a
    // few ulps away in floating point -- makes roughly half the fragments of every glowing object
    // fail their own depth test, which reads as the glow tearing into stripes that crawl as the
    // camera moves.
    gl_Position = camera.projection * camera.view * worldPos;
}
