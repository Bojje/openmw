#version 460

// One additively blended shape, drawn forward in the composite pass rather than deferred into the
// G-buffer: a light halo, the glow around a candle, a magic effect mesh.
//
// Same 48-byte interleave every other mesh in this renderer uses, so these are the very meshes the
// G-buffer would have drawn and nothing had to be uploaded twice.

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

// Must match Vk::EmissiveMeshPush and the block in emissive.frag exactly.
//   model   offset  0, 64 bytes
//   params  offset 64, 16 bytes -- .x is the sampler slot, .yzw unused
// 80 bytes, well under the guaranteed 128.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)  mat4 model;
    layout(offset = 64) uvec4 params;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec4 fragColor;
layout(location = 2) flat out uint fragTexture;
// For the underwater test in emissive.frag. Already computed here for gl_Position, so carrying it
// costs one interpolant and no arithmetic -- the same trade particle.vert makes.
layout(location = 3) out vec3 fragWorldPos;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);

    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragTexture = push.params.x;
    fragWorldPos = worldPos.xyz;

    // inNormal is declared and never used, and that is deliberate: this surface emits light rather
    // than receiving it, so there is no normal matrix here and nothing to shade. Declaring the
    // attribute anyway is what lets the pipeline keep the one shared vertex layout instead of adding
    // a second stride to the codebase that only this pass would use. The sky mesh pipeline does the
    // same thing for the same reason.
    gl_Position = camera.projection * camera.view * worldPos;
}
