#version 460

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragMaterial;
layout(location = 5) flat in uint fragMaterialFlags;
layout(location = 6) flat in uint fragAlbedoTextureIndex;
layout(location = 7) flat in uint fragAlphaTextureIndex;
layout(location = 8) in vec2 fragAlphaTexCoord;
layout(location = 9) flat in uint fragNormalTextureIndex;

layout(set = 0, binding = 1) uniform sampler2D albedoTextures[64];
layout(set = 0, binding = 2) uniform sampler2D alphaTextures[64];
layout(set = 0, binding = 3) uniform sampler2D normalTextures[64];

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
} camera;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    vec4 albedoSample = texture(albedoTextures[fragAlbedoTextureIndex], fragTexCoord);
    vec4 albedo = fragColor * albedoSample;

    if ((fragMaterialFlags & 2u) != 0u)
        albedo.a *= texture(alphaTextures[fragAlphaTextureIndex], fragAlphaTexCoord).a;

    if ((fragMaterialFlags & 1u) != 0u) {
        // The threshold is packed into the upper byte of the per-draw flag word.
        uint threshold = (fragMaterialFlags >> 8u) & 255u;
        if (albedo.a < float(threshold) / 255.0)
            discard;
    }

    outAlbedo = albedo;

    vec3 N = normalize(fragNormal);
    if ((fragMaterialFlags & 4u) != 0u)
    {
        // Terrain has no authored tangent stream. Match the legacy terrain
        // shader's fixed tangent basis and rebuild a stable world-space TBN.
        vec3 tangent = normalize(vec3(1.0, 0.0, 0.0) - N * dot(N, vec3(1.0, 0.0, 0.0)));
        if (dot(tangent, tangent) < 1e-6)
            tangent = normalize(vec3(0.0, 1.0, 0.0) - N * dot(N, vec3(0.0, 1.0, 0.0)));
        vec3 bitangent = normalize(cross(N, tangent));
        vec3 normalSample = texture(normalTextures[fragNormalTextureIndex], fragTexCoord).xyz * 2.0 - 1.0;
        N = normalize(tangent * normalSample.x + bitangent * normalSample.y + N * normalSample.z);
    }
    outNormal = vec4(N * 0.5 + 0.5, 1.0);

    outMaterial = fragMaterial;
}
