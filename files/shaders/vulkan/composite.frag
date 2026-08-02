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

// Narkowicz's fit to the ACES RRT+ODT. It expects a pre-exposure of about 0.6: without it the curve
// sits too high everywhere, and a mid-grey texture in full sun lands at display code 195 instead of
// 127. The output is display-*linear*, not display-encoded -- its slope at the origin is 0.214, where
// any gamma-encoded curve would be above 1 -- so the swapchain's sRGB store is still the right place
// for the transfer function. Do not add a pow() here as well; see trap 12.
const float sAcesPreExposure = 0.6;

vec3 acesFilmic(vec3 x) {
    x *= sAcesPreExposure;
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
        // fragTexCoord.y is 0 at the *top* of the screen in Vulkan, so it has to be flipped: using it
        // directly put the pale horizon colour at the zenith and the deep blue along the horizon.
        float t = 1.0 - fragTexCoord.y;
        outColor = vec4(mix(skyHorizon, skyTop, t), 1.0);
        return;
    }

    vec4 materialSample = texture(gbufferMaterial, fragTexCoord);

    vec3 albedo = albedoSample.rgb;
    vec3 N = normalize(normalSample.rgb * 2.0 - 1.0);
    vec3 L = normalize(-push.sunDirection.xyz);
    vec3 sunCol = push.sunColor.rgb;

    float NdotL = max(dot(N, L), 0.0);

    // Strictly 0 or 1: the shadow ray either hits (payload stays at its pre-trace zero) or misses
    // (shadow.rmiss writes w = 1). There is no penumbra and no partial occlusion, so this steps
    // straight from full sun to the flat 0.15 ambient below -- a 7x jump with hard aliased edges.
    float shadow = rtSample.r;

    float roughness = materialSample.r;

    // Reconstruct world position from depth and inverse matrices. Needed by both the reflection
    // weighting and the specular term below, so it has to come before either.
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depthSample, 1.0);
    vec4 viewPos = scene.projInverse * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos4 = scene.viewInverse * viewPos;
    vec3 worldPos = worldPos4.xyz;

    vec3 V = normalize(push.cameraPosition.xyz - worldPos);

    // raygen stores raw reflected radiance; the weighting happens here and only here.
    //
    // Roughness-aware Schlick (Lagarde's IBL form), not plain Schlick. Plain Schlick rises to 1.0 at
    // grazing incidence, and the ground is nearly always viewed at grazing incidence, so it laid a
    // bright mirror sheen over every floor and made rough dirt look like wet glass. On a real rough
    // surface microfacet shadowing suppresses that; the Smith G term would express it, but there is no
    // GGX lobe here, so cap the response at 1 - roughness instead. At the roughness 0.8 that
    // gbuffer.frag currently hardcodes, that is 0.04 head-on rising to 0.2 at the edge.
    const float F0 = 0.04; // dielectric; metals would use albedo, but nothing is metallic yet
    float grazing = pow(1.0 - max(dot(N, V), 0.0), 5.0);
    float fresnel = F0 + (max(1.0 - roughness, F0) - F0) * grazing;
    vec3 reflectionColor = rtSample.gba * fresnel;

    vec3 ambient = albedo * 0.15;
    // Energy conservation: light reflected specularly is light that did not scatter diffusely. Without
    // the (1 - fresnel) the reflection was pure additive gain on top of an already full-strength
    // diffuse term, which is the other half of why surfaces looked like they had a glowing film on top.
    vec3 diffuse = albedo * sunCol * NdotL * shadow * (1.0 - fresnel);

    // Derive the highlight from the stored roughness instead of a fixed strength. Morrowind surfaces
    // are rough (0.8 from gbuffer.frag), and a fixed narrow highlight blows out large smooth-shaded
    // faces like boulders. Roughness drives both the exponent and the intensity, so rough surfaces get
    // a broad, weak highlight rather than a tight bright one.
    float shininess = mix(128.0, 4.0, roughness);
    float specularStrength = 0.3 * (1.0 - roughness);
    vec3 H = normalize(L + V);
    // Gated on NdotL: without it a surface facing away from the sun can still catch a highlight
    // wherever the half-vector happens to align, which shows up as rim light on unlit faces.
    float spec = NdotL > 0.0 ? pow(max(dot(N, H), 0.0), shininess) : 0.0;
    vec3 specular = sunCol * spec * specularStrength * shadow;

    vec3 color = ambient + diffuse + specular + reflectionColor;

    // Tone map only. The swapchain image is a _SRGB format (see Swapchain::chooseSurfaceFormat), so the
    // hardware applies the sRGB transfer function on write. Encoding here as well double-encodes: a
    // linear 0.216 -- mid grey -- leaves this shader at 0.5 and reaches the display at 0.74, which lifts
    // the whole midtone range and reads as a pale, low-contrast image.
    color = acesFilmic(color);

    outColor = vec4(color, 1.0);
}
