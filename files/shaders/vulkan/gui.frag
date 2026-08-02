#version 460

layout(set = 0, binding = 0) uniform sampler2D diffuseMap;

layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

vec3 srgbToLinear(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    // The texture is bound through an _SRGB view, so this sample is already linear. The vertex
    // colour is not: MyGUI packs it out of skin XML and widget code as four sRGB bytes, and the
    // vertex input decodes those as UNORM, which is a straight 0-255 to 0-1 rescale and no transfer
    // function. Decoding it here is what keeps the product in one colour space.
    //
    // Alpha is coverage, not colour, and must not be decoded -- the same rule the rest of this
    // renderer follows for visibility, occlusion and attenuation.
    vec4 texel = texture(diffuseMap, fragTexCoord);
    outColor = vec4(texel.rgb * srgbToLinear(fragColor.rgb), texel.a * fragColor.a);

    // Note what this implies for blending, because it is a real behavioural difference from the OSG
    // path and not an oversight. The swapchain is an _SRGB format, so the hardware encodes on store
    // and the blend the pipeline performs therefore happens in linear space. OSG draws the interface
    // into a plain RGBA8 target, so its blend happens in gamma space. Linear blending is the more
    // defensible of the two and is consistent with everything else this renderer does, but it makes
    // partially covered pixels lighter: an antialiased glyph edge at 50% coverage over black lands
    // at display code 187 rather than 127, so text reads slightly bolder than it does under OSG.
    // If that turns out to be the wrong call, the fix is here and in the pipeline's blend factors,
    // not anywhere upstream.
}
