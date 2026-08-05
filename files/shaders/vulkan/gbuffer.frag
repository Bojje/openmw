#version 460

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragMaterial;
layout(location = 5) flat in uint fragMaterialFlags;
layout(location = 6) flat in uint fragAlbedoTextureIndex;

layout(set = 0, binding = 1) uniform sampler2D albedoTextures[64];

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

    if ((fragMaterialFlags & 1u) != 0u) {
        // The threshold is packed into the upper byte of the per-draw flag word.
        uint threshold = (fragMaterialFlags >> 8u) & 255u;
        if (albedo.a < float(threshold) / 255.0)
            discard;
    }

    outAlbedo = albedo;

    vec3 N = normalize(fragNormal);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);

    outMaterial = fragMaterial;
}
