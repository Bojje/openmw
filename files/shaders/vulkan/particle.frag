#version 460

// A particle quad, blended additively onto the finished scene.
//
// Occlusion is done here rather than by the hardware, because the composite render pass this draws in
// has one colour attachment and no depth -- see the note in vkmyguirendermanager, which reaches the
// same conclusion for the interface. The G-buffer's depth image is already a sampler on the composite
// descriptor set and the G-buffer pass already leaves it in SHADER_READ_ONLY_OPTIMAL with a dependency
// that makes it visible to fragment reads, so testing against it costs one texture fetch and no
// synchronisation at all. Re-attaching it would cost a render pass, a framebuffer per swapchain image
// and a barrier, for the same result.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in vec4 fragColour;
layout(location = 2) flat in uint fragTexture;
layout(location = 3) flat in uint fragAdditive;

layout(set = 0, binding = 1) uniform sampler2D textures[512];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

layout(location = 0) out vec4 outColor;

// The same response chain composite.frag puts the rest of the frame through, and it has to be repeated
// here rather than shared because this draws after that shader has already run.
//
// Without it the flame was the only lit thing in the image that skipped exposure and the tone curve
// entirely: every wall and floor was multiplied by four and pushed through the curve, while the fire
// went out at its authored value. That is a factor of four the wrong way and it is why the flame read
// as dim next to the OSG renderer's, and why a flickering light visibly swung against a fire that did
// not move with it.
//
// It applies to the additive effects only, and that distinction is the whole point rather than a
// shortcut. Additive means the effect emits light -- fire, sparks, magic -- so it belongs on the same
// exposure as every other light in the scene. An alpha blended effect does not emit anything; it is a
// surface tinting what is behind it, and what is behind it has already been exposed and tone mapped.
// Exposing it a second time is how a torch's smoke came out as a bright grey cloud on a dark wall
// where the OSG renderer shows a faint wisp.
//
// Tone mapping the particle on its own is not the same as tone mapping the sum, which is what an HDR
// scene target would let us do. It is deliberate: the composite pass has one attachment and it is the
// swapchain image, so the alternative is a full-resolution offscreen target and a fullscreen copy every
// frame. On a 15 W handheld that is not worth paying for a difference visible only where a flame
// already saturates.
const float sExposure = 4.0;
const float sAcesPreExposure = 0.6;
const float sTonemapStrength = 0.25;

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
    // Screen-space UV for the depth fetch. Vulkan's framebuffer origin is top-left and so is the
    // depth image's, so this needs no flip -- see trap 15, which is the same fact biting the other
    // way round.
    vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));
    float sceneDepth = texture(gbufferDepth, screenUv).r;

    // Behind opaque geometry. Discarded rather than blended, because additive blending has no depth
    // test of its own and a flame inside a wall would otherwise light it up from the far side.
    if (gl_FragCoord.z > sceneDepth)
        discard;

    vec4 texel = texture(textures[fragTexture], fragUv);

    // Soft particles. A quad that intersects the ground would otherwise cut a hard straight line
    // across it, which is the single thing that most makes billboards read as billboards. Fading
    // over the last stretch of depth before the surface behind hides that entirely.
    //
    // The comparison is in non-linear depth, which is deliberately crude: it is far more sensitive
    // near the camera, which is exactly where the artifact is visible and where the fade wants to be
    // tightest. A view-space version would need the projection constants and would look worse.
    float fade = clamp((sceneDepth - gl_FragCoord.z) * 4000.0, 0.0, 1.0);

    // Both sides of this are linear: the sampler decodes the texture because the loader gives it an
    // _SRGB format, and the reader decodes the particle colour on the way in.
    vec4 colour = texel * fragColour;

    vec3 mapped = colour.rgb;
    if (fragAdditive != 0u)
    {
        vec3 exposed = colour.rgb * sExposure;
        mapped = mix(clamp(exposed, 0.0, 1.0), acesFilmic(exposed), sTonemapStrength);
    }

    // The alpha carries the weight rather than the colour being pre-multiplied. The additive pipeline
    // blends SRC_ALPHA, ONE, so this adds colour * alpha and never darkens what is behind it; the
    // second pipeline uses the authored ONE_MINUS_SRC_ALPHA, which is what lets smoke darken a wall
    // instead of glowing on it.
    outColor = vec4(mapped, colour.a * fade);
}
