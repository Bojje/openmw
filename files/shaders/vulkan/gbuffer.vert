#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;

// Must match Vk::GBufferPushConstants and the block in gbuffer.frag exactly.
// A push-constant mat3 occupies 48 bytes: three columns, each padded out to a vec4.
//   model         offset   0, 64 bytes
//   normalMatrix  offset  64, 48 bytes
//   materialBits  offset 112,  4 bytes -- sampler slot plus the authored alpha test, unpacked in
//                                         gbuffer.frag; see Vk::packMaterialBits
//   roughness     offset 116,  4 bytes
//   specular      offset 120,  4 bytes
// The block runs to 128 bytes with boneOffset, which is exactly the guaranteed limit. This stage
// declares only as far as it needs, which is legal.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)   mat4 model;
    layout(offset = 64)  mat3 normalMatrix;
    layout(offset = 112) uint materialBits;
    layout(offset = 116) float roughness;
    layout(offset = 120) float specularStrength;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragColor;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(push.normalMatrix * inNormal);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    gl_Position = camera.projection * camera.view * worldPos;
}
