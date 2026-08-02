#version 460

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;

// Must match Vk::GBufferPushConstants and the block in gbuffer.vert exactly. See gbuffer.vert for the
// byte offsets; the block is declared identically in both stages even though each only reads part of it.
layout(push_constant) uniform PushConstants {
    layout(offset = 0)   mat4 model;
    layout(offset = 64)  mat3 normalMatrix;
    layout(offset = 112) uint textureIndex;
    layout(offset = 116) float roughness;
    layout(offset = 120) float specularStrength;
} push;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

// Fixed-size sampler array. push.textureIndex is a push constant and therefore uniform across the
// draw call, so plain indexing is valid here: no nonuniformEXT and no descriptor indexing extension.
// Slot 0 is a 1x1 white fallback used by untextured meshes.
layout(set = 0, binding = 1) uniform sampler2D textures[512];

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    vec4 albedo = texture(textures[push.textureIndex], fragTexCoord) * fragColor;

    // Alpha cutout. Morrowind's foliage is flat quads whose leaf shape lives entirely in the texture's
    // alpha channel, so without this every leaf sprite renders as an opaque rectangle. Formats without
    // alpha (BC1_RGB, and the 1x1 white fallback) sample a == 1.0 and are unaffected, so this needs no
    // per-material flag. It is a hard cutout rather than blending: the G-buffer is opaque, and sorted
    // alpha blending would need a separate pass.
    if (albedo.a < 0.5)
        discard;

    outAlbedo = albedo;

    vec3 N = normalize(fragNormal);
    outNormal = vec4(N * 0.5 + 0.5, 1.0);

    // Real per-material values, not constants. The green channel carries specular strength rather than
    // metallic: Morrowind has no metals, and upstream disables specular outright for Morrowind-era NIFs
    // (nifosg::Loader::applyDrawableProperties), so for vanilla content this is 0 and the composite
    // pass produces neither a highlight nor a reflection. That is correct -- it is what the OSG
    // renderer does -- and it is why the previous hardcoded 0.8 roughness put a sheen on the world.
    float ao = 1.0;
    float emission = 0.0;
    outMaterial = vec4(push.roughness, push.specularStrength, ao, emission);
}
