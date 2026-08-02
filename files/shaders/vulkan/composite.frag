#version 460

layout(location = 0) in vec2 fragTexCoord;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 3) uniform sampler2D rtOutput;

layout(set = 0, binding = 5) uniform sampler2D gbufferMaterial;
layout(set = 0, binding = 7) uniform sampler2D rtIndirect;
// .a is the per-pixel history length. Bound to whichever half of the ping-pong raygen wrote this
// frame, in GENERAL layout.
layout(set = 0, binding = 8) uniform sampler2D denoiseHistory;

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
    mat4 prevViewFromCurView;
    vec4 denoiseParams;
    vec4 sunParams;
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

// Scene exposure. Not a fudge factor -- it is doing the job auto-exposure would.
//
// OpenMW's light colours are authored and consumed in gamma space, and decoding them to linear for
// this pipeline shrinks them substantially: an ambient of 0.55 becomes 0.26. Combined with the fact
// that nothing here is HDR (peak radiance is barely above 1), the tone curve never leaves its toe and
// the whole image sits far darker than the OSG renderer's, which multiplies those same numbers
// directly in gamma space with no curve at all.
//
// The right fix is a real auto-exposure pass, and OpenMW already has the luminance machinery for it
// in files/shaders/lib/luminance/. Until that is wired up this constant stands in for it, and any
// future auto-exposure should replace it rather than stack on top.
const float sExposure = 2.5;

vec3 acesFilmic(vec3 x) {
    x *= sAcesPreExposure * sExposure;
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

    // --- Spatial fallback for pixels the temporal accumulator has nothing for ------------------
    //
    // Temporal accumulation does nothing for a pixel with no history. A freshly disoccluded pixel is
    // a single sample of the sun cone and a single sample of the hemisphere, and those pixels are not
    // scattered -- they cluster into a wedge trailing every occluder as the camera moves. No amount
    // of tuning the blend weight fixes it; there is nothing to blend with.
    //
    // A 5x5 cross-bilateral gather over the neighbours that lie on the same surface substitutes
    // neighbouring samples for the missing history. It is applied only where the history is short,
    // which is a small and spatially coherent minority of the frame -- about 0.65% while panning --
    // so whole quads take the early-out.
    //
    // This lives in composite rather than in the separate compute pass DENOISER-PLAN.md proposes.
    // The plan's argument for compute was that a graphics pass would need its own render pass and
    // framebuffer for an offscreen target, and that is true of a pass that rewrites the signal in
    // place: neighbours would race against the writes. Filtering here needs no target at all, because
    // composite reads the ray tracing outputs and writes the swapchain, so there is no hazard and no
    // extra image. What it gives up is feeding the filtered result back into the history, which real
    // SVGF does -- so this improves what is displayed but not what converges. The *.comp glob is in
    // cmake/CompileShaders.cmake now, so taking the feedback route later costs a pipeline, not a
    // build system change.
    float historyLength = texture(denoiseHistory, fragTexCoord).a;

    vec4 rtFiltered = rtSample;
    vec4 indirectFiltered = texture(rtIndirect, fragTexCoord);

    // Above this the temporal estimate is trusted on its own. Provisional: it trades residual noise
    // against over-blurring, and both halves of that are resolution and frame rate dependent, so it
    // wants re-checking on a Steam Deck rather than being treated as settled.
    const float sSpatialAgeThreshold = 8.0;

    if (historyLength < sSpatialAgeThreshold)
    {
        vec3 centreNormal = normalize(normalSample.rgb * 2.0 - 1.0);
        vec2 texel = 1.0 / vec2(textureSize(rtOutput, 0));

        vec4 rtSum = vec4(0.0);
        vec4 indirectSum = vec4(0.0);
        float weightSum = 0.0;

        for (int y = -2; y <= 2; ++y)
        {
            for (int x = -2; x <= 2; ++x)
            {
                vec2 tapUv = fragTexCoord + vec2(x, y) * texel;
                if (any(lessThan(tapUv, vec2(0.0))) || any(greaterThanEqual(tapUv, vec2(1.0))))
                    continue;

                // Sky has no surface to share, and its depth would pass no sane test anyway.
                float tapDepth = texture(gbufferDepth, tapUv).r;
                if (tapDepth >= 1.0)
                    continue;

                // Edge stopping on the same two quantities the temporal rejection uses, and for the
                // same reason: a neighbour is only a substitute for history if it is a sample of the
                // same surface. Without this the filter bleeds shadow across silhouettes, which is a
                // worse artifact than the noise it removes.
                vec3 tapNormal = normalize(texture(gbufferNormal, tapUv).rgb * 2.0 - 1.0);
                if (dot(centreNormal, tapNormal) <= 0.9)
                    continue;

                // Relative depth, so the tolerance means the same thing at 50 units and at 5000.
                if (abs(tapDepth - depthSample) > 0.01 * max(depthSample, 1e-5))
                    continue;

                // Gaussian-ish spatial falloff. sigma ~1.5 px over a 5x5 support.
                float d2 = float(x * x + y * y);
                float weight = exp(-d2 / 4.5);

                rtSum += texture(rtOutput, tapUv) * weight;
                indirectSum += texture(rtIndirect, tapUv) * weight;
                weightSum += weight;
            }
        }

        if (weightSum > 0.0)
        {
            // Fade the filter out as history builds, rather than switching it off at the threshold.
            // A hard cut leaves a visible seam that crawls across surfaces as pixels age past it.
            float blend = 1.0 - historyLength / sSpatialAgeThreshold;
            rtFiltered = mix(rtSample, rtSum / weightSum, blend);
            indirectFiltered = mix(indirectFiltered, indirectSum / weightSum, blend);
        }
    }

    rtSample = rtFiltered;

    // Used raw. This is now a soft, temporally accumulated estimate of the fraction of the sun's disc
    // that is visible, not the binary hit/miss it used to be, and there is no longer a floor under it.
    //
    // The floor that used to be here (sShadowFloor, 0.35) existed because a fully shadowed surface had
    // no occlusion term of any kind: the shadow dropped straight to a flat ambient and crushed to
    // black. It was documented as a placeholder for ray traced ambient occlusion plus a GI bounce,
    // to be deleted rather than tuned once either existed. AO now exists, so it is deleted. Do not
    // reintroduce it -- if shadowed surfaces look too dark, the honest fixes are the AO radius, the
    // ambient term, or the missing GI bounce, in that order.
    float shadow = rtSample.r;

    // One-bounce indirect light, from the hemisphere ray raygen traces.
    //   .rgb = the accumulated albedo of whatever that ray hit
    //   .a   = sky visibility, which is the ambient occlusion term
    //
    // The bounce arrives as an albedo rather than a radiance, deliberately: the light is applied
    // here, this frame, so weather transitions and the lightning flash cannot enter the history and
    // ghost. That is the same reason the shadow term accumulates visibility rather than radiance.
    vec3 bounceAlbedo = indirectFiltered.rgb;
    float ao = indirectFiltered.a;

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
    // dead flat -- this at least gives unlit geometry shape. The world is Z-up.
    //
    // It used to stand in for the sky occlusion a proper AO term would compute, and it no longer has
    // to: the hemisphere lerp now only expresses which way a surface faces, and `ao` expresses what
    // is actually in front of it. It stays deliberately shallow so the two do not compound.
    float hemisphere = mix(0.7, 1.0, N.z * 0.5 + 0.5);
    vec3 ambient = albedo * push.ambientColor.rgb * hemisphere * ao;

    // The bounce. This is what replaces the sShadowFloor placeholder properly: a surface in shadow is
    // lit by light that reflected off its surroundings, and until now this renderer computed none of
    // that, so a shadowed surface fell to ambient alone and crushed to black.
    //
    // albedo * bounceAlbedo is the light that left this surface after two reflections, and it is what
    // produces colour bleed -- red rock throws red onto the ground beside it, not grey. Note
    // bounceAlbedo already carries the occluded fraction: the hemisphere ray contributes zero when it
    // escapes to sky, so its accumulated value is (1 - skyVisibility) times the average albedo of
    // whatever is nearby. The occlusion and the bounce cannot disagree, because they are one ray.
    //
    // What lights the bounce surface has to include the sky, not just the sun. Weighting this by
    // sunColor alone was measurably wrong: at dusk, under overcast, and anywhere indoors, sunColor is
    // dim and nearly all the light ricocheting around a shadowed recess arrived from the sky. With
    // only the sun term the bounce lifted near-black pixels by 4.7%, which is the right shape and an
    // order of magnitude too little to replace what sShadowFloor was faking.
    //
    // The sun half is still an approximation -- it assumes the bounce surface was itself lit, and
    // settling that honestly needs a shadow ray from the bounce point, which is a third ray per
    // pixel. Half weight rather than full acknowledges that some of those surfaces are in shadow too.
    // Light does not bounce once and stop. The full series is
    //     L = L0 * (1 + a + a^2 + a^3 + ...) = L0 / (1 - a)
    // for an average surface albedo a, and this shader traces exactly one bounce, so it captures the
    // first term and drops the rest. Dividing by (1 - a) restores the tail without tracing it, which
    // is the standard cheap multi-bounce approximation and is a good deal more defensible than
    // scaling by a constant chosen to make the picture brighter.
    //
    // It matters more here than it would in most scenes: Morrowind's textures are dark, and a single
    // bounce off a dark surface returns very little. Measured against the sShadowFloor placeholder,
    // single-bounce alone left mid-shadow regions 15-23% darker than the flat 0.35 lift did.
    //
    // Clamped well below 1 because the series diverges as albedo approaches unity, and a near-white
    // modded texture would otherwise produce an enormous multiplier from a term nothing else bounds.
    const float sGiSunFactor = 0.5;
    vec3 bounceLight = push.ambientColor.rgb + sunCol * sGiSunFactor;
    vec3 multiBounce = 1.0 / (1.0 - min(bounceAlbedo, vec3(0.85)));
    vec3 indirect = albedo * bounceAlbedo * multiBounce * bounceLight;
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

    // The point lights' *ambient* contribution is occluded, their diffuse is not. Upstream adds
    // light.ambient with no NdotL, which makes it a crude local fill -- exactly the kind of
    // omnidirectional term AO is a correction for. The diffuse half already has a direction and an
    // NdotL, and these lights cast no shadow ray, so occluding it with a hemisphere-average term
    // would darken the lit side of a torch-lit wall for no defensible reason.
    vec3 color = ambient + indirect + diffuse + specular + reflectionColor
        + albedo * (pointDiffuse + pointAmbient * ao);

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
