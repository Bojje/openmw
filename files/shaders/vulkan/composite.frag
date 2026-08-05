#version 460

layout(location = 0) in vec2 fragTexCoord;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 5) uniform sampler2D gbufferMaterial;

layout(set = 0, binding = 4) uniform SceneUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
} scene;

layout(location = 0) out vec4 outColor;

vec3 acesFilmic(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 albedoSample = texture(gbufferAlbedo, fragTexCoord);
    vec4 normalSample = texture(gbufferNormal, fragTexCoord);
    vec4 materialSample = texture(gbufferMaterial, fragTexCoord);
    float depthSample = texture(gbufferDepth, fragTexCoord).r;

    if (depthSample >= 1.0) {
        vec3 skyTop = vec3(0.2, 0.4, 0.8);
        vec3 skyHorizon = vec3(0.6, 0.75, 0.9);
        float t = fragTexCoord.y;
        outColor = vec4(mix(skyHorizon, skyTop, t), 1.0);
        return;
    }

    vec3 albedo = albedoSample.rgb;
    vec3 N = normalize(normalSample.rgb * 2.0 - 1.0);
    vec3 L = normalize(-scene.sunDirection.xyz);
    vec3 sunCol = scene.sunColor.rgb;

    float NdotL = max(dot(N, L), 0.0);

    float shadow = 1.0;
    vec3 reflectionColor = vec3(0.0);

    float roughness = clamp(materialSample.r, 0.05, 1.0);
    float ao = materialSample.b;
    float emission = max(materialSample.a, 0.0);
    vec3 ambient = albedo * scene.ambientColor.rgb * ao;
    vec3 diffuse = albedo * sunCol * NdotL * shadow;

    // Reconstruct world position from depth and inverse matrices
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depthSample, 1.0);
    vec4 viewPos = scene.projInverse * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos4 = scene.viewInverse * viewPos;
    vec3 worldPos = worldPos4.xyz;

    float specularStrength = 0.3 * (1.0 - roughness);
    vec3 V = normalize(scene.viewInverse[3].xyz - worldPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), mix(128.0, 1.0, roughness));
    vec3 specular = sunCol * spec * specularStrength * shadow;

    vec3 color = ambient + diffuse + specular + reflectionColor + albedo * emission;

    color = acesFilmic(color);
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
