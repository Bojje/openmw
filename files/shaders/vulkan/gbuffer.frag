#version 460

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in vec4 fragColor;
layout(location = 4) in vec4 fragMaterial;
layout(location = 5) flat in uint fragMaterialFlags;
layout(location = 6) flat in uint fragAlbedoTextureIndex;
layout(location = 7) flat in uint fragAlphaTextureIndex;
layout(location = 8) in vec2 fragAlphaTexCoord;
layout(location = 9) flat in uint fragNormalTextureIndex;
layout(location = 10) in vec4 fragTangent;
layout(location = 11) flat in uint fragEmissiveTextureIndex;
layout(location = 12) flat in uint fragSpecularTextureIndex;
layout(location = 13) in vec4 fragEmissive;
layout(location = 14) flat in vec2 fragEmissiveLumaBias;
layout(location = 15) flat in uvec4 fragTextureLayers;
layout(location = 16) in vec2 fragDarkTexCoord;
layout(location = 17) in vec2 fragDetailTexCoord;
layout(location = 18) in vec2 fragDecalTexCoord;

layout(set = 0, binding = 1) uniform sampler2D albedoTextures[64];
layout(set = 0, binding = 2) uniform sampler2D alphaTextures[64];
layout(set = 0, binding = 3) uniform sampler2D normalTextures[64];
layout(set = 0, binding = 4) uniform sampler2D emissiveTextures[64];
layout(set = 0, binding = 5) uniform sampler2D specularTextures[64];
layout(set = 0, binding = 6) uniform sampler2D darkTextures[64];
layout(set = 0, binding = 7) uniform sampler2D detailTextures[64];
layout(set = 0, binding = 8) uniform sampler2D decalTextures[64];

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
} camera;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out vec4 outMaterial;
layout(location = 3) out vec4 outSpecular;
layout(location = 4) out vec4 outEmissive;

void main() {
    vec2 terrainTexCoord = fragTexCoord;
    vec3 N = normalize(fragNormal);
    vec3 tangent = vec3(0.0);
    vec3 bitangent = vec3(0.0);
    vec4 normalSample = vec4(0.5, 0.5, 1.0, 1.0);
    if ((fragMaterialFlags & 4u) != 0u)
    {
        if ((fragMaterialFlags & 2u) != 0u)
        {
            // Terrain has no authored tangent stream. Match the legacy terrain
            // shader's fixed tangent basis and rebuild a stable world-space TBN.
            tangent = normalize(vec3(1.0, 0.0, 0.0) - N * dot(N, vec3(1.0, 0.0, 0.0)));
            if (dot(tangent, tangent) < 1e-6)
                tangent = normalize(vec3(0.0, 1.0, 0.0) - N * dot(N, vec3(0.0, 1.0, 0.0)));
        }
        else
        {
            tangent = normalize(fragTangent.xyz - N * dot(N, fragTangent.xyz));
            if (dot(tangent, tangent) < 1e-6)
                tangent = normalize(vec3(1.0, 0.0, 0.0) - N * dot(N, vec3(1.0, 0.0, 0.0)));
        }
        bitangent = normalize(cross(N, tangent));
        if ((fragMaterialFlags & 2u) == 0u)
            bitangent *= fragTangent.w;
        normalSample = texture(normalTextures[fragNormalTextureIndex], terrainTexCoord);
        if ((fragMaterialFlags & 8u) != 0u)
        {
            vec3 viewDirection = normalize(camera.viewInverse[3].xyz - fragWorldPos);
            vec3 tangentViewDirection = vec3(dot(viewDirection, tangent), dot(viewDirection, bitangent),
                dot(viewDirection, N));
            terrainTexCoord += tangentViewDirection.xy * (normalSample.a * 0.04 - 0.02);
            normalSample = texture(normalTextures[fragNormalTextureIndex], terrainTexCoord);
        }
    }

    vec4 albedoSample = texture(albedoTextures[fragAlbedoTextureIndex], terrainTexCoord);
    vec4 albedo = fragColor * albedoSample;
    if (fragTextureLayers.x != 0u)
    {
        vec4 darkSample = texture(darkTextures[fragTextureLayers.x], fragDarkTexCoord);
        albedo *= darkSample;
    }
    vec3 emissiveSample = fragEmissiveTextureIndex == 0u
        ? vec3(0.0)
        : texture(emissiveTextures[fragEmissiveTextureIndex], terrainTexCoord).rgb;
    vec3 specularSample = fragSpecularTextureIndex == 0u
        ? vec3(1.0)
        : texture(specularTextures[fragSpecularTextureIndex], terrainTexCoord).rgb;
    if (fragMaterial.b > 1.5 && fragSpecularTextureIndex == 0u)
        specularSample = albedoSample.rgb;

    if ((fragMaterialFlags & 2u) != 0u)
        albedo.a *= texture(alphaTextures[fragAlphaTextureIndex], fragAlphaTexCoord).a;

    if ((fragMaterialFlags & 1u) != 0u) {
        // The threshold is packed into the upper byte of the per-draw flag word.
        uint threshold = (fragMaterialFlags >> 8u) & 255u;
        if (albedo.a < float(threshold) / 255.0)
            discard;
    }

    if (fragTextureLayers.y != 0u)
        albedo.rgb *= texture(detailTextures[fragTextureLayers.y], fragDetailTexCoord).rgb * 2.0;
    if (fragTextureLayers.z != 0u)
    {
        vec4 decalSample = texture(decalTextures[fragTextureLayers.z], fragDecalTexCoord);
        albedo.rgb = mix(albedo.rgb, decalSample.rgb, decalSample.a * fragColor.a);
    }

    outAlbedo = albedo;

    if ((fragMaterialFlags & 4u) != 0u)
    {
        vec3 sampledNormal = normalSample.xyz * 2.0 - 1.0;
        N = normalize(tangent * sampledNormal.x + bitangent * sampledNormal.y + N * sampledNormal.z);
    }

    // Legacy enchanted equipment treats the animated caustic layer as an
    // environment map. Rebuild its spherical reflection coordinates here so
    // the Vulkan path does not merely scroll the layer over the base UVs.
    // The layer is still intentionally isolated behind the neutral material
    // flag; ordinary emissive textures retain their authored coordinates.
    vec2 emissiveTexCoord = terrainTexCoord;
    if ((fragMaterialFlags & 128u) != 0u)
    {
        vec3 viewNormal = normalize((camera.view * vec4(N, 0.0)).xyz);
        vec3 viewVector = normalize((camera.view * vec4(fragWorldPos - camera.viewInverse[3].xyz, 0.0)).xyz);
        vec3 reflection = reflect(viewVector, viewNormal);
        float denominator = 2.0 * sqrt(reflection.x * reflection.x + reflection.y * reflection.y
            + (reflection.z + 1.0) * (reflection.z + 1.0));
        if (denominator > 1e-6)
            emissiveTexCoord = vec2(reflection.x / denominator + 0.5, reflection.y / denominator + 0.5);
    }
    if (fragEmissiveTextureIndex != 0u && (fragMaterialFlags & 128u) != 0u)
        emissiveSample = texture(emissiveTextures[fragEmissiveTextureIndex], emissiveTexCoord).rgb;
    if (fragEmissiveTextureIndex != 0u)
    {
        // The legacy enchanted-equipment path multiplies its environment
        // layer by the authored gloss map. The neutral converter carries that
        // map in the specular texture slot while the explicit neutral luma
        // bias travels alongside the draw payload.
        if ((fragMaterialFlags & 128u) != 0u)
        {
            if ((fragMaterialFlags & 4u) != 0u)
                emissiveSample *= clamp(normalSample.b * fragEmissiveLumaBias.x + fragEmissiveLumaBias.y, 0.0, 1.0);
            emissiveSample *= specularSample;
        }
    }
    vec3 emissiveColor = fragEmissive.rgb * max(fragEmissive.a, 0.0);
    if (fragEmissiveTextureIndex != 0u)
    {
        if ((fragMaterialFlags & 64u) != 0u)
            emissiveColor = fragEmissive.rgb * emissiveSample * max(fragEmissive.a, 1.0);
        else
            emissiveColor = max(fragEmissive.rgb, vec3(1.0)) * emissiveSample * max(fragEmissive.a, 1.0);
    }
    outNormal = vec4(N * 0.5 + 0.5, 1.0);
    outSpecular = vec4(specularSample, albedo.a);
    outEmissive = vec4(emissiveColor, 1.0);

    outMaterial = fragMaterial;
    if (fragEmissiveTextureIndex != 0u)
        outMaterial.a = max(outMaterial.a, max(max(emissiveSample.r, emissiveSample.g), emissiveSample.b));
    if (fragMaterial.b > 1.5)
        outMaterial.g = albedoSample.a;
    else if (fragSpecularTextureIndex != 0u)
        outMaterial.b = 3.0;
    if ((fragMaterialFlags & 16u) != 0u)
        outMaterial.b = 2.5;
    if ((fragMaterialFlags & 32u) != 0u)
        outMaterial.b = 4.0;
    if ((fragMaterialFlags & 64u) != 0u)
    {
        outMaterial.b = 5.0;
        outSpecular = vec4(fragMaterial.rgb, albedo.a);
    }
    if ((fragMaterialFlags & 128u) != 0u)
        outMaterial.a = 2.0;
}
