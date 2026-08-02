#version 460

layout(location = 0) in vec2 fragTexCoord;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 3) uniform sampler2D rtOutput;

layout(set = 0, binding = 5) uniform sampler2D gbufferMaterial;

layout(set = 0, binding = 4) uniform SceneUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} scene;

layout(push_constant) uniform PushConstants {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 cameraPosition;
} push;

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
    float depthSample = texture(gbufferDepth, fragTexCoord).r;
    vec4 rtSample = texture(rtOutput, fragTexCoord);

    if (depthSample >= 1.0) {
        vec3 skyTop = vec3(0.2, 0.4, 0.8);
        vec3 skyHorizon = vec3(0.6, 0.75, 0.9);
        float t = fragTexCoord.y;
        outColor = vec4(mix(skyHorizon, skyTop, t), 1.0);
        return;
    }

    vec4 materialSample = texture(gbufferMaterial, fragTexCoord);

    vec3 albedo = albedoSample.rgb;
    vec3 N = normalize(normalSample.rgb * 2.0 - 1.0);
    vec3 L = normalize(-push.sunDirection.xyz);
    vec3 sunCol = push.sunColor.rgb;

    float NdotL = max(dot(N, L), 0.0);

    float shadow = rtSample.r;

    // Attenuate the reflection by surface roughness instead of adding it flat. Morrowind surfaces are
    // overwhelmingly rough and diffuse, so an unattenuated reflection term washes the whole scene in
    // whatever colour the hit shader returns.
    float roughness = materialSample.r;
    vec3 reflectionColor = rtSample.gba * (1.0 - roughness);

    vec3 ambient = albedo * 0.15;
    vec3 diffuse = albedo * sunCol * NdotL * shadow;

    // Reconstruct world position from depth and inverse matrices
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depthSample, 1.0);
    vec4 viewPos = scene.projInverse * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos4 = scene.viewInverse * viewPos;
    vec3 worldPos = worldPos4.xyz;

    // Derive the highlight from the stored roughness instead of a fixed strength. Morrowind surfaces
    // are rough (0.8 from gbuffer.frag), and a fixed narrow highlight blows out large smooth-shaded
    // faces like boulders. Roughness drives both the exponent and the intensity, so rough surfaces get
    // a broad, weak highlight rather than a tight bright one.
    float shininess = mix(128.0, 4.0, roughness);
    float specularStrength = 0.3 * (1.0 - roughness);
    vec3 V = normalize(push.cameraPosition.xyz - worldPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), shininess);
    vec3 specular = sunCol * spec * specularStrength * shadow;

    vec3 color = ambient + diffuse + specular + reflectionColor;

    // Tone map only. The swapchain image is a _SRGB format (see Swapchain::chooseSurfaceFormat), so the
    // hardware applies the sRGB transfer function on write. Encoding here as well double-encodes: a
    // linear 0.216 -- mid grey -- leaves this shader at 0.5 and reaches the display at 0.74, which lifts
    // the whole midtone range and reads as a pale, low-contrast image.
    color = acesFilmic(color);

    outColor = vec4(color, 1.0);
}
