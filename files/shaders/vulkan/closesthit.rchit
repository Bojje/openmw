#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec4 payload;

hitAttributeEXT vec2 attribs;

void main() {
    vec3 barycentrics = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);

    vec3 hitNormal = vec3(0.0, 1.0, 0.0);

    payload = vec4(hitNormal, gl_HitTEXT);
}
