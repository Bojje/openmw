#version 460

layout(location = 0) in vec2 fragTexCoord;

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 5) uniform sampler2D gbufferMaterial;
layout(set = 0, binding = 6) uniform sampler2D gbufferSpecular;

layout(set = 0, binding = 4) uniform SceneUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
    vec4 ambientColor;
    vec4 fogColor;
    vec4 fogParameters;
    vec4 skyColor;
    vec4 effectTime;
    vec4 pointLightPositions[16];
    vec4 pointLightColorsAndRadii[16];
    vec4 pointLightCount;
} scene;

layout(location = 0) out vec4 outColor;

vec3 acesFilmic(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec4 albedoSample = texture(gbufferAlbedo, fragTexCoord);
    vec4 normalSample = texture(gbufferNormal, fragTexCoord);
    vec4 materialSample = texture(gbufferMaterial, fragTexCoord);
    vec3 specularColor = texture(gbufferSpecular, fragTexCoord).rgb;
    float depthSample = texture(gbufferDepth, fragTexCoord).r;

    if (depthSample >= 1.0) {
        vec3 skyHorizon = scene.skyColor.rgb;
        vec3 skyTop = mix(skyHorizon, vec3(0.2, 0.4, 0.8), 0.45);
        float t = fragTexCoord.y;
        if (scene.effectTime.y > 0.5)
            skyHorizon = mix(skyHorizon, scene.fogColor.rgb, 0.75);
        outColor = vec4(mix(skyHorizon, skyTop, t), 1.0);
        return;
    }

    vec3 albedo = albedoSample.rgb;
    vec3 N = normalize(normalSample.rgb * 2.0 - 1.0);
    vec3 L = normalize(-scene.sunDirection.xyz);
    vec3 sunCol = scene.sunColor.rgb;

    float NdotL = max(dot(N, L), 0.0);

    float shadow = 1.0;
    vec3 reflectionColor = vec3(0.0);

    float roughness = clamp(materialSample.r, 0.05, 1.0);
    bool terrainSpecular = materialSample.b > 1.5 && materialSample.b < 2.5;
    bool waterSurface = abs(materialSample.b - 2.5) < 0.01;
    bool objectSpecular = materialSample.b > 2.5 && materialSample.b < 3.5;
    bool ambientOverride = materialSample.b > 3.5;
    float ao = ambientOverride ? 1.0 : clamp(materialSample.b, 0.0, 1.0);
    float emission = max(materialSample.a, 0.0);
    vec3 ambient = albedo * (ambientOverride ? vec3(1.0) : scene.ambientColor.rgb * ao);
    vec3 diffuse = albedo * sunCol * NdotL * shadow;
    if (scene.effectTime.y > 0.5)
        diffuse *= 0.35;

    // Reconstruct world position from depth and inverse matrices
    vec2 ndc = fragTexCoord * 2.0 - 1.0;
    vec4 clipPos = vec4(ndc, depthSample, 1.0);
    vec4 viewPos = scene.projInverse * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos4 = scene.viewInverse * viewPos;
    vec3 worldPos = worldPos4.xyz;

    if (waterSurface)
    {
        vec2 wavePosition = worldPos.xz * 0.018;
        float waveTime = scene.effectTime.x;
        float waveA = sin(wavePosition.x * 1.7 + waveTime * 0.8);
        float waveB = sin(wavePosition.y * 2.1 - waveTime * 0.55);
        vec3 waveNormal = normalize(vec3(waveA * 0.16 + waveB * 0.08, 1.0, waveB * 0.16 - waveA * 0.08));
        N = normalize(mix(N, waveNormal, 0.35));
    }

    vec3 pointAmbient = vec3(0.0);
    vec3 pointDiffuse = vec3(0.0);
    int pointLightTotal = int(scene.pointLightCount.x);
    for (int i = 0; i < pointLightTotal; ++i)
    {
        vec3 toLight = scene.pointLightPositions[i].xyz - worldPos;
        float distanceToLight = length(toLight);
        float radius = scene.pointLightColorsAndRadii[i].w;
        if (distanceToLight <= 0.0001 || distanceToLight >= radius)
            continue;

        vec3 pointLightDirection = toLight / distanceToLight;
        float attenuation = 1.0 - distanceToLight / radius;
        vec3 pointLightColor = scene.pointLightColorsAndRadii[i].rgb;
        pointAmbient += albedo * pointLightColor * attenuation * 0.25;
        pointDiffuse += albedo * pointLightColor * max(dot(N, pointLightDirection), 0.0) * attenuation;
    }

    float specularStrength = objectSpecular ? 1.0
        : terrainSpecular ? materialSample.g : 0.3 * (1.0 - roughness);
    vec3 V = normalize(scene.viewInverse[3].xyz - worldPos);
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), mix(128.0, 1.0, roughness));
    vec3 specular = specularColor * sunCol * spec * specularStrength * shadow;
    if (scene.effectTime.y > 0.5)
        specular *= 0.15;

    vec3 color = ambient + pointAmbient + diffuse + pointDiffuse + specular + reflectionColor + albedo * emission;

    if (waterSurface)
    {
        float fresnel = pow(1.0 - clamp(dot(N, V), 0.0, 1.0), 3.0);
        vec3 reflectedSky = mix(scene.skyColor.rgb, scene.sunColor.rgb,
            pow(max(dot(reflect(-L, N), V), 0.0), 32.0));
        color = mix(color, reflectedSky, 0.25 + 0.35 * fresnel);
    }

    if (scene.effectTime.y > 0.5)
        color = mix(color, scene.fogColor.rgb, 0.18);

    if (scene.fogParameters.y > scene.fogParameters.x && scene.fogParameters.y > 0.0)
    {
        float distanceToCamera = length(worldPos - scene.viewInverse[3].xyz);
        float fogFactor = clamp((scene.fogParameters.y - distanceToCamera)
                / (scene.fogParameters.y - scene.fogParameters.x), 0.0, 1.0);
        color = mix(scene.fogColor.rgb, color, fogFactor);
    }

    color = acesFilmic(color);
    color = pow(color, vec3(1.0 / 2.2));

    outColor = vec4(color, 1.0);
}
