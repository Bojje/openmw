#version 460

// paintClouds and paintAtmosphereNight, ported out of files/shaders/compatibility/sky.frag lines 21-35,
// which is what the OSG backend draws these two meshes with.
//
// Ported rather than reinvented for the same reason the moon composite next door was. Neither of these
// is complicated on its own, but between them they carry four multiplications by four different fades
// -- vertex alpha, the opacity uniform, the material emission and the fog mix -- and the picture stays
// plausible if any one of them is dropped or applied in the wrong order. It just stays plausible in a
// way that no longer matches the other renderer, which is the failure this whole reader architecture
// exists to make impossible.

layout(location = 0) in vec2 fragUv;
layout(location = 1) in float fragAlpha;

layout(set = 0, binding = 1) uniform sampler2D textures[512];
layout(set = 1, binding = 2) uniform sampler2D gbufferDepth;

// Declared only as far as fogColor, which is all this needs, and declared identically in skymesh.vert
// -- see the note there. A uniform block may stop short of the buffer behind it as long as every
// member it does declare sits at the offset the buffer has it at, so the fields above fogColor are
// here to place it and for no other reason. Vk::SceneData is the authority on that layout and its
// static_asserts pin it.
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
} scene;

// Identical to the block in skymesh.vert. See there for why this is a push constant and not a buffer.
layout(push_constant) uniform SkyMeshPush {
    mat4 model;
    vec4 emission;
    vec4 uvOffset;
    uvec4 params;
} mesh;

// skyutil.cpp's Pass enum, which files/shaders/lib/sky/passes.glsl mirrors for the OSG shaders. Only
// the one value is needed here: the night sky is everything this does not treat as cloud.
const uint sPassClouds = 2u;

layout(location = 0) out vec4 outColor;

void main() {
    // Screen-space UV for the depth fetch. Vulkan's framebuffer origin is top-left and so is the depth
    // image's, so this needs no flip.
    vec2 screenUv = gl_FragCoord.xy / vec2(textureSize(gbufferDepth, 0));

    // Drawn only where nothing else was, exactly as sky.frag does it and for the same reason: the
    // composite pass has no depth attachment, so the only depth test available is this one, and the
    // sky is behind everything. A cleared depth of 1.0 is the same test composite.frag uses to decide
    // a pixel is sky, so this is that rule stated once more rather than a new one.
    //
    // It matters more for the meshes than it did for the sun. These are domes wrapped around the
    // camera at a radius of a few thousand units, which is nearer than half the terrain in an
    // exterior; a depth comparison would paint the cloud layer over any mountain further away than
    // the dome and produce a skyline that moves with the player.
    if (texture(gbufferDepth, screenUv).r < 1.0)
        discard;

    vec4 colour = texture(textures[mesh.params.x], fragUv);

    // Both passes fade the same way: the vertex alpha times the opacity uniform. For the clouds that
    // opacity is the weather blend -- 1 - blend on the current layer and blend on the one being
    // crossfaded to -- and for the night sky it is mNightFade * mGlareView, which is what takes the
    // stars out at dawn. Neither is recomputed here; both are read off the stateset the update
    // traversal wrote them to.
    colour.a *= fragAlpha * mesh.emission.a;

    if (mesh.params.y == sPassClouds) {
        // The clouds are tinted by the material's emission, which SkyManager sets to the weather's fog
        // colour plus 0.13 on rgb (sky.cpp lines 799-808). That is what makes a cloud layer take the
        // colour of the sunset behind it instead of staying white all day.
        //
        // The product is taken in linear, because the emission arrives already decoded and the texture
        // is sampled through an _SRGB view. OpenMW multiplies the same two numbers in gamma space, so
        // this is the systematic difference composite.frag documents at length rather than a
        // divergence peculiar to the sky, and the clamp lands in a different place because of it.
        colour.rgb = clamp(colour.rgb * mesh.emission.rgb, 0.0, 1.0);

        // "ease transition between clear color and atmosphere/clouds", in the words of the shader this
        // comes from. Toward the horizon the vertex alpha goes to zero and the cloud colour goes to the
        // fog colour, so the layer meets the haze instead of ending on a visible line. The fog colour
        // comes from the scene uniform rather than a push constant: it is the same value composite.frag
        // paints the bottom of the sky gradient with, and the two agreeing is exactly what makes the
        // horizon read as continuous.
        colour = mix(vec4(scene.fogColor.rgb, colour.a), colour, fragAlpha);
    }

    // Not premultiplied, unlike sky.frag. The sky's render bin switches GL_BLEND on with the default
    // SRC_ALPHA / ONE_MINUS_SRC_ALPHA (sky.cpp line 366) and neither of these two meshes overrides it
    // -- only the moons do -- so the pipeline uses the authored factors directly and there is nothing
    // to fold.
    outColor = colour;
}
