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
    // Sampler slot in the low ten bits, the authored alpha test in the high ones. Packed rather than
    // given fields of its own because the block is already exactly 128 bytes, which is the smallest
    // maxPushConstantsSize Vulkan guarantees. Vk::packMaterialBits builds it and owns the layout;
    // the two unpacks below are the only readers.
    layout(offset = 112) uint materialBits;
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

// Fixed-size sampler array. The slot comes out of a push constant and is therefore uniform across
// the draw call, so plain indexing is valid here: no nonuniformEXT and no descriptor indexing
// extension. Slot 0 is a 1x1 white fallback used by untextured meshes.
layout(set = 0, binding = 1) uniform sampler2D textures[512];

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;

void main() {
    vec4 albedo = texture(textures[push.materialBits & 0x3FFu], fragTexCoord) * fragColor;

    // Alpha cutout. Morrowind's foliage is flat quads whose leaf shape lives entirely in the texture's
    // alpha channel, so without this every leaf sprite renders as an opaque rectangle. It is a hard
    // cutout rather than blending: the G-buffer is opaque, and sorted alpha blending would need a
    // separate pass. Formats without alpha (BC1_RGB, and the 1x1 white fallback) sample a == 1.0, so
    // an opaque or untextured shape passes either branch below untouched.
    //
    // Two branches, and which one a shape takes is the whole point of this change.
    //
    // A shape that authored an alpha test gets the function and threshold it asked for. Nothing in
    // Morrowind.bsa does -- all 3099 of its NiAlphaProperty records are blend-only with a threshold of
    // zero, because Morrowind relied on sorted blending and left the D3D alpha test off. Tribunal and
    // Bloodmoon between them author 1140, nearly all GEQUAL at 192/255 = 0.753. Tested at 0.5 instead,
    // a band of texels the author meant to drop survives fully opaque, and every pine on Solstheim
    // wears a halo; the other 131 are GREATER at 100/255 = 0.392, which 0.5 erodes instead.
    //
    // Everything else keeps the flat 0.5, and that default is load-bearing rather than left over.
    // Vanilla depends on blending this pass cannot do, so the cutout is what stands in for it -- and
    // it is also the only thing covering the foliage that ships with no NiAlphaProperty at all and
    // relies on its texture's alpha alone. Removing it "to be faithful to the flags" would put every
    // leaf billboard in the game back to an opaque rectangle. The flags say nothing here; the texture
    // has to.
    bool keep;
    if ((push.materialBits & 0x08000000u) != 0u)
    {
        float threshold = float((push.materialBits >> 16) & 0xFFu) / 255.0;
        uint func = (push.materialBits >> 24) & 0x7u;

        // glAlphaFunc order, the same numbering NiAlphaProperty::alphaTestMode returns and nifosg's
        // getTestMode switches on (nifloader.cpp:1925-1949). The fragment is kept where the
        // comparison holds, which is what glAlphaFunc means. Uniform across the draw, so this is a
        // branch the whole wave takes together rather than divergence.
        if (func == 0u) keep = true;                        // ALWAYS
        else if (func == 1u) keep = albedo.a <  threshold;  // LESS
        else if (func == 2u) keep = albedo.a == threshold;  // EQUAL
        else if (func == 3u) keep = albedo.a <= threshold;  // LEQUAL
        else if (func == 4u) keep = albedo.a >  threshold;  // GREATER
        else if (func == 5u) keep = albedo.a != threshold;  // NOTEQUAL
        else if (func == 6u) keep = albedo.a >= threshold;  // GEQUAL
        else keep = false;                                  // NEVER
    }
    else
    {
        keep = albedo.a >= 0.5;
    }

    if (!keep)
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
