#version 460

// The water surface. Ported from files/shaders/compatibility/water.frag, minus everything that needs
// a render target this renderer does not have: no reflection map, no refraction map, no ripple
// simulation, no rain. What is left is the part that carries the look -- six scrolling octaves of one
// normal map, exact dielectric Fresnel, a sky reflection, sun glitter, and depth-derived opacity.
//
// The reflection is real now: raygen.rgen intersects this same plane analytically and fires one ray
// per 2x2 pixels off the wave normal, and what arrives here is finished radiance. See the block there
// for why that is cheaper than it sounds -- the water is not in the acceleration structure, so the
// plane costs no geometry and the ray cannot come back and hit the surface it left.
//
// What that buys over the OSG renderer is the point of the exercise. OSG reflects by re-rendering the
// world into a mirrored render target at reduced detail, and by default it leaves actors out of the
// reflection entirely. One ray reflects everything in the acceleration structure, at the same detail
// it is drawn at, for one 640x400 target on a Steam Deck at 1280x800.

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform SceneUBO {
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
    // .x is the sun's angular radius, which raygen owns. The other three were the documented spare of
    // this vec4 and are now .y = seconds, .z = the water plane's world Z, .w = 1 when there is a water
    // plane at all. They went here rather than into a new field because SceneData's offsets past
    // denoiseParams are pinned by static_asserts and mirrored by hand in three shaders.
    vec4 sunParams;
    uint frameIndex;
    uint lightCount;
    uint isInterior;
    // Slot of textures/omw/water_nm.png in the sampler array above. It used to be a push constant on
    // this pipeline; it moved into the block that was scenePad1 when raygen.rgen started building the
    // same wave normal, because two shaders reading the slot from two different places is a bug
    // waiting for the day they disagree. Occupying the pad rather than adding a field keeps every
    // offset where the static_asserts in vkrenderer.hpp pin them.
    uint waterNormalMap;
} scene;

layout(set = 0, binding = 1) uniform sampler2D textures[1024];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;
// Half resolution, written by raygen.rgen. .rgb is finished reflected radiance and .a says whether the
// ray tracing pass ran at all. Bound on the composite set beside the G-buffer depth this shader
// already samples, so the pipeline gains no descriptor plumbing of its own.
layout(set = 1, binding = 9) uniform sampler2D waterReflection;

// Tweakables, straight from water.frag lines 13-48. Names and values unchanged so the two can be
// compared line by line.
const float VISIBILITY = 2500.0;
const float DEPTH_FADE = 0.15;
const float WAVE_CHOPPYNESS = 0.05;
const float WAVE_SCALE = 75.0;
const float BUMP = 0.5;
const float SPEC_HARDNESS = 256.0;
const float SPEC_BUMPINESS = 5.0;
const float SPEC_BRIGHTNESS = 1.5;
const vec2 WIND_DIR = vec2(0.5, -0.8);
const float WIND_SPEED = 0.2;
// Upstream's water.frag has this as vec3(0.090195, 0.115685, 0.12745), which is a gamma-space value
// because that shader never leaves gamma space. Here it is blended against a scene that is linear and
// written to an _SRGB swapchain image, so using it raw hands the hardware a number it will encode a
// second time -- roughly a factor of three too bright, measured as a water region mean of 106 against
// the OSG capture's 60 in the same frame at Seyda Neen. Decoded once, here, rather than in the shader
// body: it is a constant.
const vec3 WATER_COLOR = vec3(0.008574, 0.012638, 0.014831);

vec2 normalCoords(vec2 uv, float scale, float speed, float time, float timer1, float timer2, vec3 previousNormal)
{
    return uv * (WAVE_SCALE * scale) + WIND_DIR * time * (WIND_SPEED * speed)
        - (previousNormal.xy / previousNormal.zz) * WAVE_CHOPPYNESS + vec2(time * timer1, time * timer2);
}

// Verbatim from files/shaders/lib/water/fresnel.glsl. Copied rather than included because the Vulkan
// shaders are compiled by glslangValidator straight off disk with no include path, where the GL side
// goes through OpenMW's own preprocessor.
float fresnel_dielectric(vec3 incoming, vec3 normal, float eta)
{
    float c = abs(dot(incoming, normal));
    float g = eta * eta - 1.0 + c * c;
    float result;

    if (g > 0.0)
    {
        g = sqrt(g);
        float A =(g - c)/(g + c);
        float B =(c *(g + c)- 1.0)/(c *(g - c)+ 1.0);
        result = 0.5 * A * A *(1.0 + B * B);
    }
    else
    {
        result = 1.0;  /* TIR (no refracted component) */
    }

    return result;
}

// Distance in front of the camera for a depth buffer value, in world units.
//
// Derived, not fitted. Vk::glToVulkanProjection (vkmath.hpp:203-215) turns OSG's GL projection into
// one with P[2][2] = -f/(f-n) and P[3][2] = -fn/(f-n), and it leaves the bottom row alone, so
// w_clip is still -z_view. Then d = (P[2][2]*z_view + P[3][2]) / -z_view rearranges to
// -z_view = P[3][2] / (d + P[2][2]). The check worth doing if this ever looks wrong is that it gives
// the near plane at d = 0 and the far plane at d = 1; it does.
//
// GLSL's m[col][row] matches Vk::Mat4::data[col*4 + row], so these two are data[14] and data[10].
// Reading them transposed produces a plausible-looking number that is wrong everywhere, which is the
// failure mode to watch for.
float viewDist(float d)
{
    return scene.projection[3][2] / (d + scene.projection[2][2]);
}

void main() {
    // Vulkan's framebuffer origin is top left and so is the depth image's, so this needs no flip --
    // the same fact particle.frag relies on.
    vec2 screen = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));
    float sceneDepth = texture(gbufferDepth, screen).r;

    // The composite pass has no depth attachment, so nothing tests this for us. Without the discard
    // the sea draws straight through the cliff in front of it. Same fetch and same reason as
    // particle.frag, and it also guarantees the water column below is never negative.
    if (gl_FragCoord.z > sceneDepth)
        discard;

    // World position by intersecting the view ray with the water plane rather than interpolating one
    // across the grid.
    //
    // Not a stylistic choice. The grid is 1.2 million units across and Morrowind's world runs to
    // +/-250,000, so an interpolated world position near the camera is two seven-digit numbers
    // cancelling down to a three-digit one -- and fp32 has 24 bits of mantissa. The waves would
    // quantise into steps that crawl as the camera moves. The ray costs two matrix multiplies and is
    // accurate wherever the camera happens to be standing.
    vec2 ndc = screen * 2.0 - 1.0;
    vec4 rayView = scene.projInverse * vec4(ndc, 1.0, 1.0);
    vec3 rayDir = normalize((scene.viewInverse * vec4(rayView.xyz / rayView.w, 0.0)).xyz);
    vec3 cameraPos = scene.viewInverse[3].xyz;

    float waterHeight = scene.sunParams.z;

    // The denominator is zero exactly along the horizon, where the plane is edge on. Those pixels are
    // far past the fog end and contribute nothing; what the guard buys is that they land a long way
    // away rather than at infinity, because an infinity here turns the wave lookups into NaN and a
    // NaN alpha is a hole in the frame rather than a subtle error.
    float denom = rayDir.z;
    if (abs(denom) < 1.0e-6)
        denom = denom < 0.0 ? -1.0e-6 : 1.0e-6;
    float toPlane = clamp((waterHeight - cameraPos.z) / denom, 0.0, 1.0e7);
    vec3 worldPos = cameraPos + rayDir * toPlane;

    float time = scene.sunParams.y;
    vec2 UV = worldPos.xy / (8192.0 * 5.0) * 3.0;

    // Six octaves of one normal map, each octave's lookup displaced by the previous octave's normal.
    // That feedback is the whole effect. Without it the six layers slide over each other at six
    // constant speeds and the sea reads as sheets of texture on a conveyor belt -- which is exactly
    // what it looks like if the previousNormal argument is simplified away. Scales, speeds and the
    // two per-octave timers are unchanged from water.frag:104-109.
    //
    // The seed is (0,0,1) where upstream passes (0,0,0). normalCoords divides xy by zz, so a zero seed
    // is 0.0/0.0. It yields the same offset either way when it does not produce a NaN, and there is no
    // reason to keep the coin flip.
    vec3 seed = vec3(0.0, 0.0, 1.0);
    vec3 normal0 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 0.05, 0.04, time, -0.015, -0.005, seed)).rgb - 1.0;
    vec3 normal1 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 0.1, 0.08, time, 0.02, 0.015, normal0)).rgb - 1.0;
    vec3 normal2 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 0.25, 0.07, time, -0.04, -0.03, normal1)).rgb - 1.0;
    vec3 normal3 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 0.5, 0.09, time, 0.03, 0.04, normal2)).rgb - 1.0;
    vec3 normal4 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 1.0, 0.4, time, -0.02, 0.1, normal3)).rgb - 1.0;
    vec3 normal5 = 2.0 * texture(textures[scene.waterNormalMap], normalCoords(UV, 2.0, 0.7, time, 0.1, -0.06, normal4)).rgb - 1.0;

    // Every octave weighted 0.1, which is what BIG_WAVES, MID_WAVES and SMALL_WAVES all come to with
    // no rain. Rain ripples and the ripple map are not ported: both read simulation state that lives
    // on the OSG side and neither is visible at the distance most water is seen from.
    vec3 normal = (normal0 + normal1 + normal2 + normal3 + normal4 + normal5) * 0.1;
    normal = normalize(vec3(-normal.x * BUMP, -normal.y * BUMP, normal.z));

    // rayDir already points from the eye into the surface, which is the direction water.frag builds as
    // normalize(position - cameraPos).
    vec3 viewDir = rayDir;

    bool below = cameraPos.z < waterHeight;

    // Air to water above the surface, water to air below it. Upstream compares against 0 because its
    // water geometry sits at local z = 0 and a transform carries the height; here the plane is at its
    // world height, so that is what the comparison is against. Getting it backwards makes the surface
    // stop reflecting the moment the player wades in.
    float ior = below ? (1.0 / 1.333) : (1.333 / 1.0);
    float fresnel = clamp(fresnel_dielectric(viewDir, normal, ior), 0.0, 1.0);

    // The reflection, traced by raygen.rgen against this same plane. What comes back is already lit,
    // exposed, tone mapped and fogged over the leg from the surface out to whatever it hit, so all
    // that is left here is to weight it by Fresnel and blend.
    //
    // Half a full-resolution texel of offset on the lookup, and it is not cosmetic. raygen samples at
    // the top-left pixel of each 2x2 block, but the texel it writes is addressed from that block's
    // centre -- so a plain fetch reads the answer for a point half a pixel away, and the whole
    // reflection creeps up and to the left by that much. The shift puts the bilinear weights back over
    // the pixels the rays were actually fired from.
    vec2 halfResFix = 0.5 / vec2(textureSize(gbufferDepth, 0));
    vec4 reflectSample = texture(waterReflection, screen + halfResFix);

    vec3 reflectDir = reflect(viewDir, normal);

    // The reflected direction is recomputed here rather than read back, and it will not be bit-identical
    // to the one raygen reflected off: that shader has no derivatives, so it picks the normal map's mip
    // level analytically where this one gets it from the rasteriser. Both are the same wave field
    // filtered to the pixel; they differ in filter width at distance. What matters is that the constants
    // and the octave chain match, because that is what keeps the reflection sitting on the waves you can
    // see rather than sliding across them.

    // .a is 1 wherever raygen ran and 0 otherwise -- and it is 0 for the whole image or for none of it,
    // because raygen writes every texel it launches over, including the ones with no water under them.
    // So this is a per-frame switch, not a per-pixel blend, and a bilinear fetch can never straddle the
    // two. It covers the frames before the first cell has an acceleration structure, and any build or
    // device without ray tracing at all.
    //
    // The fallback is what this shader drew everywhere until now. composite.frag renders OpenMW's whole
    // atmosphere as one lerp between the fog colour and the sky colour along the vertical (lines
    // 112-113), because that is all the sky dome is -- a flat colour whose alpha ramps out at the
    // horizon over a clear colour set to the fog colour. Reflecting it is the same lerp on the
    // reflected direction, and it tracks weather, sunrise and ash storms for free because both
    // endpoints already do.
    vec3 skyFallback = mix(scene.fogColor.rgb, scene.skyColor.rgb, clamp(reflectDir.z, 0.0, 1.0));
    vec3 reflection = reflectSample.a > 0.5 ? reflectSample.rgb : skyFallback;

    // From below, total internal reflection past about 49 degrees turns the underside of the sea into
    // a mirror, and what it mirrors is the underwater world this renderer does not draw. The water
    // colour stands in for it. The alternative -- leaving the sky reflection in place -- puts the sky
    // underneath the surface, which reads as a hole in the sea rather than as water.
    if (below)
        reflection = WATER_COLOR;

    float surfaceDist = viewDist(gl_FragCoord.z);

    // How much water the view ray passes through before it reaches whatever is underneath. This is the
    // number the old arrangement could not produce at all: with the surface in the G-buffer, the depth
    // at a water pixel *was* the surface and the seabed was not recorded anywhere.
    //
    // Sky behind the water means the ray escaped to the far plane with no seabed on it. The honest
    // answer there is fully opaque; treating it as zero depth instead makes the sea transparent along
    // the horizon and you see the sky through it, which is the single most obvious way this can look
    // wrong.
    float column = (sceneDepth >= 1.0) ? 1.0e6 : (viewDist(sceneDepth) - surfaceDist);

    // water.frag:197-198 verbatim. It reaches 0 at zero depth and 1 at VISIBILITY, with the knee
    // DEPTH_FADE puts in between; the discard above is what keeps the denominator away from its pole,
    // which sits at about -55 units of water.
    //
    // This replaces both of upstream's shore treatments -- its depth fade and its wobbly-shore hack --
    // with the one number. The wobble exists to hide the fact that a refraction texture sampled with a
    // distorted offset reads the wrong depth near a shore; there is no refraction texture here, so
    // there is nothing to hide.
    float depthCorrection = sqrt(1.0 + 4.0 * DEPTH_FADE * DEPTH_FADE);
    float factor = DEPTH_FADE * DEPTH_FADE / (-0.5 * depthCorrection + 0.5 - column / VISIBILITY)
        + 0.5 * depthCorrection + 0.5;
    float depthAlpha = clamp(factor, 0.0, 1.0);

    // From below there is no column between the eye and the surface for this to describe, and this
    // renderer has no underwater fog to hand the job to. The surface is then Fresnel and nothing else,
    // which is why looking straight up out of the water shows the sky and looking along it does not.
    if (below)
        depthAlpha = 0.0;

    // Premultiplied alpha, blended ONE / ONE_MINUS_SRC_ALPHA.
    //
    // The surface reflects `fresnel` of what is in front of it and transmits the rest; what it
    // transmits is the seabed seen through `depthAlpha` worth of water colour. Written this way the
    // blend unit produces
    //     fresnel * reflection + (1 - fresnel) * mix(seabed, WATER_COLOR, depthAlpha)
    // exactly, which is water.frag:199 with the reflection put back on top of it.
    //
    // Straight SRC_ALPHA / ONE_MINUS_SRC_ALPHA cannot express that: it multiplies the reflection by
    // the same alpha as the tint, so an inch of clear water over sand stops reflecting the sky -- and
    // reflecting the sky at a grazing angle is the most recognisable thing shallow water does. Nor is
    // adding the reflection on afterwards a fix; that double-counts light and blows out deep water at
    // a grazing angle, where both terms are near 1.
    float alpha = 1.0 - (1.0 - fresnel) * (1.0 - depthAlpha);
    vec3 colour = fresnel * reflection + (1.0 - fresnel) * depthAlpha * WATER_COLOR;

    // Sun glitter, ported whole from water.frag:164-170. SPEC_MAGIC is not derivable from anything --
    // it came from the Blender shader the original was written against, and its own comment says
    // moving it either kills the highlight or floods the screen with it.
    //
    // No shadow term and no sun-visibility fade. Both of those live on the G-buffer pixel behind the
    // water, which is the seabed rather than the surface, so there is nothing here to read them from.
    // The highlight therefore survives into cloud shadow, which is a smaller lie than no highlight.
    const float SPEC_MAGIC = 1.55;
    vec3 sunWorldDir = normalize(-scene.sunDirection.xyz);
    vec3 specNormal = normalize(vec3(normal.x * SPEC_BUMPINESS, normal.y * SPEC_BUMPINESS, normal.z));
    float phongTerm = max(dot(reflect(viewDir, specNormal), sunWorldDir), 0.0);
    float specular = clamp(pow(atan(phongTerm * SPEC_MAGIC), SPEC_HARDNESS) * SPEC_BRIGHTNESS, 0.0, 1.0);

    // Fog, because leaving the G-buffer means composite.frag no longer applies it for us. Copied from
    // composite.frag:444-446: planar distance along the view axis and a linear ramp, which is what
    // OpenMW's defaults are -- radial and exponential fog are both off in settings-default.cfg.
    //
    // Without this the sea stays sharp and coloured all the way to the far plane while the land beside
    // it fades out, which is more obviously wrong than the flat sheet this replaced.
    float fogRange = max(scene.fogParams.y - scene.fogParams.x, 1.0);
    float fogValue = clamp((surfaceDist - scene.fogParams.x) / fogRange, 0.0, 1.0);

    // Applied to the premultiplied colour, and the fog colour is premultiplied to match. At full fog
    // the background behind this pixel is already the fog colour, so the surface has to contribute
    // exactly fogColor * alpha for the blend to land back on fogColor. Mixing the finished
    // premultiplied value toward an unpremultiplied fog colour instead over-brightens every distant
    // wave, in a band that follows the horizon.
    colour = mix(colour, scene.fogColor.rgb * alpha, fogValue);

    // The glitter is a light rather than a surface colour, so it adds instead of tinting, and it fades
    // out with distance rather than toward the fog colour.
    colour += specular * scene.sunColor.rgb * (1.0 - fogValue);

    outColor = vec4(colour, alpha);
}
