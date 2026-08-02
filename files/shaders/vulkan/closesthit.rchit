#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

hitAttributeEXT vec2 attribs;

void main() {
    // TODO: look up the hit surface's albedo. Doing that needs the vertex/index buffers and a material
    // table addressable from the hit shader (via gl_InstanceCustomIndexEXT), none of which are wired up
    // yet. Until then return a neutral grey: this shader previously returned vec3(0.0, 1.0, 0.0) -- an
    // up-vector placeholder -- which raygen reads as reflectionColor, tinting the whole scene green.
    vec3 hitColor = vec3(0.5);

    payload = vec4(hitColor, gl_HitTEXT);
}
