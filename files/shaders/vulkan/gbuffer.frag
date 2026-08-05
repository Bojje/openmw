#version 460

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    outAlbedo = fragColor;

    vec3 N = normalize(fragNormal);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);

    float roughness = 0.8;
    float metallic = 0.0;
    float ao = 1.0;
    float emission = 0.0;
    outMaterial = vec4(roughness, metallic, ao, emission);
}
