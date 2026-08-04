#version 460

// The sun disc and the two moons, composited onto the finished scene.
//
// paintSun and paintMoon are ported line for line out of files/shaders/compatibility/sky.frag, which
// is what the OSG backend draws these with. Ported rather than reinvented, and the moon composite is
// the reason: it is Morrowind's original two-pass blend -- a shadow pass with normal alpha, then a
// phase pass added on top -- folded into one premultiplied pass, and which factor multiplies which is
// not recoverable by looking at the images.

layout(location = 0) in vec2 fragUv;

layout(set = 0, binding = 1) uniform sampler2D textures[1024];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

// Identical to the block in sky.vert. See there for why this is a push constant and not a buffer.
layout(push_constant) uniform SkyPush {
    vec4 position;
    vec4 right;
    vec4 up;
    vec4 colour;
    vec4 moonBlend;
    vec4 atmosphereFade;
    uvec4 params;
} sky;

// The same block sky.vert declares. See there for why the answer needs no smoothing.
layout(set = 1, binding = 10) readonly buffer SunVisibilityBuffer {
    vec4 discDir;
    uvec4 params;
    uint totalRays;
    uint visibleRays;
    uint sunVisPad0;
    uint sunVisPad1;
} sunVis;

float visibleRatio() {
    if (sunVis.totalRays == 0u)
        return 1.0;

    return float(sunVis.visibleRays) / float(sunVis.totalRays);
}

layout(location = 0) out vec4 outColor;

// paintMoon below is a port of a shader that never leaves gamma space, and it has to be run there.
// Its two textures arrive *linear*, because the loader gives them an _SRGB format and the sampler
// decodes on read, while moonBlend and atmosphereFade are read straight off the OSG stateset and are
// gamma. Multiplying one by the other mixes the two spaces, and because the products do not commute
// with the transfer function the result is far too bright: a mask of 0.8 gamma against a fade of 0.37
// gives 0.30 in display terms upstream and 0.51 here. That is why the moons were near-solid discs on a
// night sky where the OSG image shows almost nothing.
//
// So: encode the samples back to gamma, composite exactly as upstream does, and decode the result for
// the _SRGB attachment. Alpha is untouched -- the alpha channel of an _SRGB format is already linear.
vec3 srgbEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

vec3 srgbDecode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

void main() {
    // The glare, and it comes first because it takes none of the rest of this shader: no
    // texture, no UV, and above all no depth test.
    //
    // paintSunglare is two lines -- the colour is the material's emission and the alpha is its
    // diffuse alpha (files/shaders/compatibility/sky.frag lines 69-73) -- blended SRC_ALPHA /
    // ONE over the framebuffer. That is gamma-space arithmetic on a gamma-space framebuffer,
    // and this attachment is _SRGB. So the product is formed in gamma exactly as upstream forms
    // it, clamped exactly where upstream's fixed-function pipeline clamps it, and only then
    // decoded for the attachment. sky.colour.rgb is deliberately still gamma when it arrives:
    // the reader hands it over undecoded precisely so this multiply can happen on the right
    // side of the transfer function. Decoding it on the CPU and scaling the linear value here
    // is the mistake this comment exists to prevent -- it is a different colour, not a rounding
    // difference, because the fade is the thing being multiplied in.
    //
    // The alpha written out is zero, and that is what makes the blend additive. The sky pipeline
    // blends ONE / ONE_MINUS_SRC_ALPHA; a source alpha of zero turns the destination factor into
    // one, so the result is dst + src. A second pipeline differing only in a blend factor is a
    // second pipeline that has to be kept identical in every other respect, which the note on
    // the moons' blend already says at more length.
    //
    // What this does NOT reproduce is the space the *blend* happens in. Upstream adds into a
    // gamma framebuffer; an _SRGB attachment decodes, adds and re-encodes, so this add is
    // linear. The two agree exactly over black and diverge as the destination brightens -- half
    // strength over a mid-grey sky lands near 0.68 in display terms where upstream saturates to
    // 1.0. Reproducing it exactly would mean reading the destination, which means an input
    // attachment and a subpass self-dependency on the pixel the interface is about to draw over.
    // That was not worth it: a linear add is also what light actually does, and the two things
    // the task names as Morrowind behaviours -- the angle falloff and the doubled colour -- are
    // reproduced exactly.
    if (sky.params.z == 3u) {
        float fade = sky.colour.a * visibleRatio();
        outColor = vec4(srgbDecode(clamp(sky.colour.rgb * fade, 0.0, 1.0)), 0.0);
        return;
    }

    // The sun flash. Also before the depth test, and for a reason worth stating: createSunFlash
    // switches GL_DEPTH_TEST off (skyutil.cpp line 816) and puts the quad in RenderBin_SunGlare,
    // so upstream draws this halo over the world rather than behind it. That is not an
    // oversight -- it is what lets the flash stay a whole circle while the sun sinks behind a
    // ridge, shrinking under SunFlashCallback's scale instead of being sliced in half by the
    // ridge line. Give this the sun disc's "only where the depth is still cleared" test and the
    // effect reads as broken exactly when it is supposed to be at its most dramatic.
    if (sky.params.z == 2u) {
        // The same two lines paintSun runs, because upstream draws the flash through the same
        // PASS_SUN branch. No gamma work anywhere here: the texture is decoded by the sampler
        // and both factors are scalars, so nothing mixes the two spaces.
        vec4 flash = texture(textures[sky.params.x], fragUv) * sky.colour;

        // SunFlashCallback's fade band, and it is the exact algebra of that callback rather
        // than a lookalike. Below a tenth of the disc showing it overrides the material with an
        // alpha of fade * mGlareView, where fade ramps 0 to 1 across that tenth; at or above a
        // tenth it uses no override, so the alpha is the sun transform's own material diffuse
        // alpha -- which is mGlareView again, and which the reader has already put in
        // sky.colour.a. min(1, ratio * 10) is those two branches written as one expression.
        flash.a *= clamp(visibleRatio() * 10.0, 0.0, 1.0);

        // Premultiplied on the way out, like the sun disc, so the one pipeline covers both.
        outColor = vec4(flash.rgb * flash.a, flash.a);
        return;
    }

    // Screen-space UV for the depth fetch. Vulkan's framebuffer origin is top-left and so is the depth
    // image's, so this needs no flip -- see trap 15, which is the same fact biting the other way round.
    vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));

    // Drawn only where nothing else was, because the sky is behind everything.
    //
    // NOT the depth comparison particle.frag does, and getting this wrong is silent. The sun sits 1000
    // world units from the camera -- CelestialBody::mDistance -- which is nearer than most terrain, so
    // comparing this fragment's depth against the scene's would happily paint the sun over a mountain
    // six thousand units away. A cleared depth of 1.0 is exactly the test composite.frag already uses
    // to decide a pixel is sky, so this is the same rule stated once more rather than a new one.
    if (texture(gbufferDepth, screenUv).r < 1.0)
        discard;

    vec4 colour;

    // Was `!= 0`, which was correct while 0 and 1 were the only values. params.z now carries
    // four modes -- see Vk::SkyMode -- and the two above have already returned, so this could
    // stay as it was and be right by accident. It is spelled out instead, because the next mode
    // added is the one that turns a wrong sun into a moon.
    if (sky.params.z == 1u) {
        vec4 phase = texture(textures[sky.params.x], fragUv);
        vec4 mask = texture(textures[sky.params.y], fragUv);

        // Morrowind does this in two passes
        //
        // First pass: moon shadow, normal blending (src alpha, 1 - src alpha)
        // dst.rgb = mask.rgb * mask.a + dst.rgb * (1 - mask.a)
        // Second pass: moon phase, additive blending (src alpha, 1)
        // dst.rgb += phase.rgb * phase.a
        //
        // The same is doable in a single pass through premultiplied alpha blending
        // color.rgb = mask.rgb * mask.a + phase.rgb * phase.a
        // color.a = mask.a
        // dst.rgb = color.rgb + dst.rgb * (1 - color.a)
        vec3 maskTinted = srgbEncode(mask.rgb) * sky.atmosphereFade.rgb;
        float maskAlpha = mask.a * sky.atmosphereFade.a;
        vec3 phaseTinted = srgbEncode(phase.rgb) * sky.moonBlend.rgb;
        float phaseAlpha = phase.a * sky.atmosphereFade.a;

        colour.rgb = srgbDecode(maskTinted * maskAlpha + phaseTinted * phaseAlpha);
        colour.a = maskAlpha;
    } else {
        // paintSun: the colour is the texture and the alpha is the texture's times the material's
        // diffuse alpha, which the reader has already put in sky.colour.a. sky.colour.rgb is white,
        // because paintSun tints by nothing -- see the note in vkskyreader.cpp on why the sun
        // material's emission is deliberately not used here.
        colour = texture(textures[sky.params.x], fragUv) * sky.colour;

        // Premultiplied on the way out, so one pipeline draws both.
        //
        // The sun in OSG blends SRC_ALPHA / ONE_MINUS_SRC_ALPHA (the default, with GL_BLEND switched on
        // at the sky's render bin root, sky.cpp line 366) and the moons override it with
        // ONE / ONE_MINUS_SRC_ALPHA (MoonUpdater::setDefaults, skyutil.cpp line 344). Multiplying the
        // sun's colour by its own alpha here makes the first identical to the second, which is worth a
        // multiply: the alternative is a second pipeline differing in one blend factor, and two
        // pipelines that must agree in every other respect are two pipelines that can drift.
        colour.rgb *= colour.a;
    }

    outColor = colour;
}
