#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

void main() {
    vec3 direction = normalize(gl_WorldRayDirectionEXT);
    // Morrowind's world is Z-up, so the horizon-to-zenith ramp runs along z. Using y here made the
    // gradient run north-to-south instead, giving straight up and straight down the same colour.
    float t = 0.5 * (direction.z + 1.0);

    vec3 horizon = vec3(0.6, 0.75, 0.9);
    vec3 zenith = vec3(0.2, 0.4, 0.8);
    vec3 skyColor = mix(horizon, zenith, t);

    payload = vec4(skyColor, -1.0);
}
