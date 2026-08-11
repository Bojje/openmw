#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inMaterial;
layout(location = 5) in vec2 inBlendTexCoord;
layout(location = 6) in vec4 inTangent;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat3 normalMatrix;
    uint materialFlags;
    uint textureIndices;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
} camera;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out vec4 fragColor;
layout(location = 4) out vec4 fragMaterial;
layout(location = 5) flat out uint fragMaterialFlags;
layout(location = 6) flat out uint fragAlbedoTextureIndex;
layout(location = 7) flat out uint fragAlphaTextureIndex;
layout(location = 8) out vec2 fragAlphaTexCoord;
layout(location = 9) flat out uint fragNormalTextureIndex;
layout(location = 10) out vec4 fragTangent;
layout(location = 11) flat out uint fragEmissiveTextureIndex;
layout(location = 12) flat out uint fragSpecularTextureIndex;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(mat3(push.normalMatrix) * inNormal);
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragMaterial = inMaterial;
    fragMaterialFlags = push.materialFlags;
    fragAlbedoTextureIndex = push.textureIndices & 63u;
    fragAlphaTextureIndex = (push.textureIndices >> 6u) & 63u;
    fragNormalTextureIndex = (push.textureIndices >> 12u) & 63u;
    fragEmissiveTextureIndex = (push.textureIndices >> 18u) & 63u;
    fragSpecularTextureIndex = (push.textureIndices >> 24u) & 63u;
    fragAlphaTexCoord = inBlendTexCoord;
    fragTangent = vec4(normalize(mat3(push.model) * inTangent.xyz), inTangent.w);
    gl_Position = camera.projection * camera.view * worldPos;
}
