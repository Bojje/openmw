#version 460

// The sun disc and the two moons, composited onto the finished scene.
//
// paintSun and paintMoon are ported line for line out of files/shaders/compatibility/sky.frag, which
// is what the OSG backend draws these with. Ported rather than reinvented, and the moon composite is
// the reason: it is Morrowind's original two-pass blend -- a shadow pass with normal alpha, then a
// phase pass added on top -- folded into one premultiplied pass, and which factor multiplies which is
// not recoverable by looking at the images.

layout(location = 0) in vec2 fragUv;

layout(set = 0, binding = 1) uniform sampler2D textures[512];
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

    if (sky.params.z != 0u) {
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
