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
    vec4 ambientColor;
    vec4 skyColor;
    vec4 fogColor;
    vec4 fogParams; // x = fog start, y = fog end, both world units
    mat4 prevViewProjection;
    uint frameIndex;
    uint lightCount;
    uint scenePad0;
    uint scenePad1;
} scene;

// Mirrors Vk::PointLight. 64 bytes, std430; the layout is pinned by static_asserts on the C++ side.
struct PointLight {
    vec3 position;   // world space
    float radius;
    vec3 diffuse;
    float attenuationConstant;
    vec3 ambient;
    float attenuationLinear;
    vec3 specular;
    float attenuationQuadratic;
};

layout(set = 0, binding = 6, std430) readonly buffer Lights { PointLight lights[]; } pointLights;

layout(push_constant) uniform PushConstants {
    vec4 sunDirection;
    vec4 sunColor;
    vec4 cameraPosition;
    vec4 ambientColor;
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

    // Reconstruct the world-space view ray. Needed for the sky, and cheaper than it looks because the
    // same inverse matrices are used for the world position below.
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 rayView = scene.projInverse * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize((scene.viewInverse * vec4(rayView.xyz / rayView.w, 0.0)).xyz);

    if (depthSample >= 1.0) {
        // This is OpenMW's entire atmosphere model. Its sky dome is a single flat colour whose vertex
        // alpha ramps to zero at the horizon, composited over a clear colour set to the fog colour --
        // so the whole thing reduces to one lerp along the vertical. Both endpoints already carry the
        // current weather and time of day, which is why this tracks sunrise and ash storms for free.
        //
        // Derived from the view ray rather than screen position: the old gradient used fragTexCoord.y,
        // which welded it to the viewport, so looking up gave the same image as looking ahead. The
        // world is Z-up.
        float up = clamp(rayDir.z, 0.0, 1.0);
        outColor = vec4(mix(scene.fogColor.rgb, scene.skyColor.rgb, up), 1.0);
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
    // Scaled by the material's own specular strength, which is 0 for all vanilla Morrowind content.
    // A surface that does not reflect specularly does not reflect the world either, so this gates the
    // ray traced reflection as well as the highlight below.
    float specularStrength = materialSample.g;
    float fresnel = (F0 + (max(1.0 - roughness, F0) - F0) * grazing) * specularStrength;
    vec3 reflectionColor = rtSample.gba * fresnel;

    // Morrowind's per-cell mood colour, not a flat grey constant. This is the single biggest thing
    // separating a Dwemer ruin from an Ashlander yurt, and a constant threw all of it away.
    //
    // Applied as a hemisphere rather than uniformly: full ambient on upward-facing surfaces, dimmed on
    // downward-facing ones. A uniform fill gives every shadowed surface the same value, which reads as
    // dead flat -- this at least gives unlit geometry shape. It stands in for the sky occlusion an
    // ambient occlusion or GI term would compute properly. The world is Z-up.
    float hemisphere = mix(0.45, 1.0, N.z * 0.5 + 0.5);
    vec3 ambient = albedo * push.ambientColor.rgb * hemisphere;
    // Energy conservation: light reflected specularly is light that did not scatter diffusely. Without
    // the (1 - fresnel) the reflection was pure additive gain on top of an already full-strength
    // diffuse term, which is the other half of why surfaces looked like they had a glowing film on top.
    vec3 diffuse = albedo * sunCol * NdotL * shadow * (1.0 - fresnel);

    // Blinn-Phong rather than GGX on purpose: vanilla Morrowind and the OSG renderer both use
    // Blinn-Phong, so this matches the reference. The exponent is the inverse of the roughness
    // parametrisation MeshConverter applied to NiMaterialProperty::mGlossiness.
    float shininess = 2.0 / max(roughness * roughness * roughness * roughness, 1e-4) - 2.0;
    vec3 H = normalize(L + V);
    // Gated on NdotL: without it a surface facing away from the sun can still catch a highlight
    // wherever the half-vector happens to align, which shows up as rim light on unlit faces.
    float spec = NdotL > 0.0 ? pow(max(dot(N, H), 0.0), shininess) : 0.0;
    vec3 specular = sunCol * spec * specularStrength * shadow;

    // Point lights. Morrowind's interiors are built almost entirely from these, and without them an
    // interior is just a directional sun shining through solid walls.
    //
    // The attenuation reproduces SceneUtil::configureLight plus calcAttenuation from
    // files/shaders/lib/light/util.glsl: the coefficients come from the light itself (Morrowind's
    // stock fallbacks give constant 0, linear 3/radius, quadratic 0) and there is a smooth cutoff over
    // the outer 25% of the radius. Physical inverse-square would make every torch wrong.
    //
    // The ambient term is not an oversight: upstream adds light.ambient * attenuation with no NdotL,
    // which is why a torch-lit room reads soft rather than spotlit. World-placed lights carry zero
    // ambient, but a light carried in inventory carries white, so it has to be per light.
    vec3 pointDiffuse = vec3(0.0);
    vec3 pointAmbient = vec3(0.0);
    for (uint i = 0u; i < scene.lightCount; ++i)
    {
        PointLight light = pointLights.lights[i];
        vec3 toLight = light.position - worldPos;
        float dist = length(toLight);
        if (dist > light.radius)
            continue;

        float atten = 1.0 / max(light.attenuationConstant + light.attenuationLinear * dist
                + light.attenuationQuadratic * dist * dist, 1e-4);

        // fade(x) = 1 - (1 - x^2)^2 over the outer quarter of the radius, matching util.glsl.
        float edge = clamp((dist / light.radius - 0.75) / 0.25, 0.0, 1.0);
        float oneMinusSq = 1.0 - edge * edge;
        atten *= 1.0 - (1.0 - oneMinusSq * oneMinusSq);

        vec3 lightDir = toLight / max(dist, 1e-4);
        pointDiffuse += light.diffuse * max(dot(N, lightDir), 0.0) * atten;
        pointAmbient += light.ambient * atten;
    }

    vec3 color = ambient + diffuse + specular + reflectionColor
        + albedo * (pointDiffuse + pointAmbient);

    // Tone map only. The swapchain image is a _SRGB format (see Swapchain::chooseSurfaceFormat), so the
    // hardware applies the sRGB transfer function on write. Encoding here as well double-encodes: a
    // linear 0.216 -- mid grey -- leaves this shader at 0.5 and reaches the display at 0.74, which lifts
    // the whole midtone range and reads as a pale, low-contrast image.
    color = acesFilmic(color);

    // Fog after the tone map, not before. OpenMW mixes toward the fog colour on the pre-transfer value
    // and never tone maps at all, so mixing in linear beforehand would put the midpoint somewhere else
    // entirely and distant terrain would come out too dark and too saturated. Applying it here on the
    // display-linear result reproduces the reference curve closely, at the cost of the fog itself not
    // being tone mapped -- which is the right trade when the goal is matching the mood.
    //
    // Planar distance along the view axis, and a linear ramp, because that is what OpenMW's defaults
    // are: radial fog and exponential fog are both off in settings-default.cfg.
    float fogRange = max(scene.fogParams.y - scene.fogParams.x, 1.0);
    float fogValue = clamp((abs(viewPos.z) - scene.fogParams.x) / fogRange, 0.0, 1.0);
    color = mix(color, scene.fogColor.rgb, fogValue);

    outColor = vec4(color, 1.0);
}
