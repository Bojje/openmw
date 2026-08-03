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

layout(set = 0, binding = 1) uniform sampler2D textures[512];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

layout(location = 0) out vec4 outColor;

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

    vec4 colour = texel * fragColour;

    // Additive, and the alpha carries the weight rather than the colour being pre-multiplied. The
    // pipeline blends SRC_ALPHA, ONE, so this adds colour * alpha and never darkens what is behind
    // it. Fire, sparks and magic all want that; smoke does not, and will want its own pipeline
    // variant with ONE_MINUS_SRC_ALPHA when it arrives.
    outColor = vec4(colour.rgb, colour.a * fade);
}
