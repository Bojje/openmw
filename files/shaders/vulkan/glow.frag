#version 460

layout(location = 0) in vec2 fragEnvUv;
layout(location = 1) flat in vec3 fragColour;
layout(location = 2) flat in uint fragTexture;

layout(set = 0, binding = 1) uniform sampler2D textures[512];

// The composite pass has no depth attachment, so this compares against the G-buffer's depth the
// same way particle.frag does.
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

layout(push_constant) uniform PushConstants {
    layout(offset = 0)   mat4 model;
    layout(offset = 64)  mat3 normalMatrix;
    layout(offset = 112) vec3 glowColour;
    layout(offset = 124) uint textureIndex;
} push;

layout(location = 0) out vec4 outColor;

// The glow's arithmetic is upstream's and upstream never leaves gamma space, so it is run there and
// the result decoded for this attachment. Same treatment, and for the same reason, as the moons in
// sky.frag: the caustic texture arrives *linear*, because the loader gives it an _SRGB format and
// the sampler decodes on read, while the enchantment colour is read straight off the OSG stateset
// and is gamma. Multiplying one by the other mixes two spaces in one product, and because the
// products do not commute with the transfer function the result comes out too bright -- a caustic
// of 0.8 against a colour of 0.6 gives 0.48 in display terms upstream and 0.66 taken linear.
vec3 srgbEncode(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308)));
}

vec3 srgbDecode(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

void main() {
    // Hand-rolled depth test. This pass draws the same triangles the G-buffer already drew, so the
    // far side of the object is in this draw too and would otherwise glow straight through the near
    // side -- an enchanted helmet would show the caustics on the inside of its own back.
    //
    // The comparison is `>` with no epsilon, exactly as particle.frag has it, and that only works
    // because glow.vert computes gl_Position with the same expression gbuffer.vert does. See the
    // note there before changing either.
    vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));
    float sceneDepth = texture(gbufferDepth, screenUv).r;
    if (gl_FragCoord.z > sceneDepth)
        discard;

    vec3 caustic = texture(textures[fragTexture], fragEnvUv).rgb;

    // objects.frag line 202, verbatim in gamma space:
    //     envEffect = texture2D(envMap, envTexCoordGen).xyz * envMapColor.xyz * envLuma
    // envLuma is 1.0 for everything here -- it is only ever anything else when the material has a
    // bump map, and the glow is added to items, which in Morrowind-era content do not have one.
    vec3 effect = srgbDecode(srgbEncode(caustic) * fragColour);

    // No exposure and no tone map, unlike the additive particles. That difference is the point
    // rather than an omission: particle.frag applies the scene's exposure because an additive
    // particle emits light and belongs on the same scale as every other light in the scene. This
    // does not emit light -- upstream adds it after lighting, on top of a finished fragment
    // (objects.frag line 240) -- so it is already in output terms. Running it through the same
    // sExposure of 4.0 would put an enchanted dagger four stops over everything around it and blow
    // it to a white silhouette.
    //
    // Alpha zero with a blend of ONE, ONE: the colour adds and the attachment's alpha is left as
    // the composite wrote it.
    outColor = vec4(effect, 0.0);
}
