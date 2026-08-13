#version 460

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inColor;
layout(location = 4) in vec4 inMaterial;
layout(location = 5) in vec2 inBlendTexCoord;
layout(location = 6) in vec4 inTangent;
layout(location = 7) in vec4 inEmissive;
layout(location = 8) in uvec4 inTextureLayers;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat3 normalMatrix;
    uint materialFlags;
    uint textureIndices;
    vec2 emissiveLumaBias;
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
layout(location = 13) out vec4 fragEmissive;
layout(location = 14) flat out vec2 fragEmissiveLumaBias;
layout(location = 15) flat out uvec4 fragTextureLayers;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(mat3(push.normalMatrix) * inNormal);
    if ((push.materialFlags & 256u) != 0u)
    {
        // Particle conversion stores each local center in the tangent
        // payload. Expand its local quad in the camera's world-space basis.
        vec3 center = (push.model * vec4(inTangent.xyz, 1.0)).xyz;
        worldPos = vec4(center + camera.viewInverse[0].xyz * inPosition.x
                + camera.viewInverse[1].xyz * inPosition.y, 1.0);
        fragWorldPos = worldPos.xyz;
        fragNormal = normalize(-camera.viewInverse[2].xyz);
    }
    fragTexCoord = inTexCoord;
    fragColor = inColor;
    fragMaterial = inMaterial;
    fragMaterialFlags = push.materialFlags;
    fragAlbedoTextureIndex = push.textureIndices & 63u;
    fragAlphaTextureIndex = (push.textureIndices >> 6u) & 63u;
    fragNormalTextureIndex = (push.textureIndices >> 12u) & 63u;
    fragEmissiveTextureIndex = (push.textureIndices >> 18u) & 63u;
    fragSpecularTextureIndex = (push.textureIndices >> 24u) & 63u;
    fragEmissive = inEmissive;
    fragEmissiveLumaBias = push.emissiveLumaBias;
    fragTextureLayers = inTextureLayers;
    fragAlphaTexCoord = inBlendTexCoord;
    fragTangent = vec4(normalize(mat3(push.model) * inTangent.xyz), inTangent.w);
    gl_Position = camera.projection * camera.view * worldPos;
}
