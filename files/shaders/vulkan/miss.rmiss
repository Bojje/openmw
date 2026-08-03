#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

// The same block raygen.rgen declares, spelled out in full rather than shortened to the two fields this
// stage reads: a miss shader is a separate stage with its own interface, and a block that stops early
// still has to place the fields it does declare at the right offsets.
layout(set = 0, binding = 5) uniform SceneUBO {
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
    vec4 sunParams;
    uint frameIndex;
    uint lightCount;
    uint isInterior;
    uint scenePad1;
} camera;

void main() {
    vec3 direction = normalize(gl_WorldRayDirectionEXT);

    // The weather's sky and fog colours rather than a hardcoded pair of blues, and this is the same
    // lerp composite.frag draws the visible sky with, so a reflected sky matches the sky beside it.
    // Before this, a ray that escaped the scene came back with a clear midday sky whatever the weather
    // and whatever the hour: reflections and the one-bounce ambient stayed blue at sunset, through a
    // storm, and underground.
    //
    // Morrowind's world is Z-up, so the horizon-to-zenith ramp runs along z. Using y here made the
    // gradient run north-to-south instead, giving straight up and straight down the same colour.
    //
    // clamp, not the old 0.5 * (z + 1) remap, again to match composite.frag: below the horizon there is
    // no sky, only fog, and a ray going down should not come back half lit by the zenith.
    float up = clamp(direction.z, 0.0, 1.0);
    vec3 skyColor = mix(camera.fogColor.rgb, camera.skyColor.rgb, up);

    payload = vec4(skyColor, -1.0);
}
