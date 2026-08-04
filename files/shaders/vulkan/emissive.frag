#version 460

// An additively blended shape, added onto the finished scene.
//
// Occlusion is done here rather than by the hardware, because the composite render pass this draws in
// has one colour attachment and no depth. The G-buffer's depth image is already a sampler on the
// composite descriptor set and the G-buffer pass leaves it in SHADER_READ_ONLY_OPTIMAL with a
// dependency that makes it visible to fragment reads, so testing against it costs one fetch and no
// synchronisation. particle.frag and water.frag reach the same three lines from the same constraint.
//
// Nothing here is lit, and that is not a shortcut -- it is what makes this pass affordable at all. A
// destination blend factor of GL_ONE says the surface only ever adds, so it emits rather than
// receives; there is no diffuse term to compute, no shadow to look up, and no sort, because addition
// commutes. An over-blended surface has none of those properties, which is why it stays in the
// G-buffer with a cutout instead of being drawn here.

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) in vec4 fragColor;
layout(location = 2) flat in uint fragTexture;
layout(location = 3) in vec3 fragWorldPos;

layout(push_constant) uniform PushConstants {
    layout(offset = 0)  mat4 model;
    layout(offset = 64) uvec4 params;
} push;

layout(set = 0, binding = 1) uniform sampler2D textures[1024];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

// Only sunParams and viewInverse are read, but a uniform block has to be declared out to the last
// member used or the offsets do not line up. Mirrors Vk::SceneData; the layout is pinned by
// static_asserts on the C++ side and by nothing at all here.
layout(set = 1, binding = 4) uniform SceneUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 skyColor;
    vec4 fogColor;
    vec4 fogParams;
    mat4 prevViewFromCurView;
    vec4 denoiseParams;
    vec4 sunParams; // .z = water plane world Z, .w = 1 when there is a water plane
    uint frameIndex;
    uint lightCount;
    uint isInterior;
    uint scenePad1;
} scene;

layout(location = 0) out vec4 outColor;

// The same response chain composite.frag puts the rest of the frame through, and the same one
// particle.frag applies to its additive quads. It has to be repeated rather than shared because this
// draws after composite.frag has already run, and because the Vulkan shaders are compiled straight
// off disk with no include path -- see the note in water.frag.
//
// It applies here for exactly the reason particle.frag gives for applying it to additive particles
// and not to blended ones: additive means the effect emits light, so it belongs on the same exposure
// as every other light in the scene. Skipped, a candle's halo would be the only emitter in the frame
// at its authored value while every wall around it was multiplied by four and curved -- a factor of
// four the wrong way, and the exact mismatch that made flames read dim before particle.frag gained
// this.
//
// These three constants now appear in three files. Retuning one means retuning all three.
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
    // Vulkan's framebuffer origin is top-left and so is the depth image's, so this needs no flip.
    vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));
    float sceneDepth = texture(gbufferDepth, screenUv).r;

    // Behind opaque geometry. Discarded rather than blended: additive blending has no depth test of
    // its own, so a halo inside a wall would otherwise light it up from the far side.
    if (gl_FragCoord.z > sceneDepth)
        discard;

    // Both sides of this product are linear. The texture is decoded by the sampler because the loader
    // gives every scene texture an _SRGB format, and the vertex colour was decoded on the way in by
    // MeshConverter::processGeometry, which runs srgbToLinear over rgb and passes alpha through as the
    // coverage scalar it is. Nothing in this file is ported from files/shaders/compatibility/, which
    // is gamma-space arithmetic throughout -- so there is no space mismatch to reconcile here, and any
    // future edit that borrows from that directory has to decode on the way in.
    vec4 colour = texture(textures[fragTexture], fragTexCoord) * fragColor;

    // Attenuate anything on the far side of the water surface from the camera.
    //
    // The depth test above cannot see the water: it is a forward draw in this same pass, so the depth
    // buffer at a water pixel holds the seabed, and a glow on the far bank of a lake would otherwise
    // pass the test and shine through at full brightness. For a flat plane the test is exact -- the
    // surface and the camera are on opposite sides exactly when their signed heights differ in sign --
    // so this needs no sorting. A flat factor rather than a path length, matching particle.frag: the
    // sea reaches full opacity in about 2500 units, which most of these are well past, and a
    // suggestion of the light is closer to what you see than removing it.
    float waterFade = 1.0;
    if (scene.sunParams.w > 0.0
        && (fragWorldPos.z - scene.sunParams.z) * (scene.viewInverse[3].z - scene.sunParams.z) < 0.0)
        waterFade = 0.15;

    vec3 exposed = colour.rgb * sExposure;
    vec3 mapped = mix(clamp(exposed, 0.0, 1.0), acesFilmic(exposed), sTonemapStrength);

    // The alpha carries the weight rather than the colour being premultiplied, because the pipeline
    // blends SRC_ALPHA, ONE. So this adds colour * alpha and can never darken what is behind it,
    // which is the defining property of the blend mode these shapes were authored with.
    //
    // No soft-particle fade, unlike particle.frag. That fade exists because a camera-facing billboard
    // cuts a hard line where it intersects the ground; these are authored meshes sitting where their
    // author put them, and fading them near geometry would thin out a halo pressed against its own
    // lamp -- which is most of them.
    outColor = vec4(mapped, colour.a * waterFade);
}
