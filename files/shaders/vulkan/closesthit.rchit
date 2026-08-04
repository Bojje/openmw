#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_nonuniform_qualifier : require

layout(location = 0) rayPayloadInEXT vec4 payload;

hitAttributeEXT vec2 attribs;

layout(buffer_reference, std430) readonly buffer Vertices { float v[]; };
layout(buffer_reference, std430) readonly buffer Indices  { uint  i[]; };

// Mirrors Vk::GeometryRecord in components/vk/vkgeometry.hpp, which static_asserts these offsets.
// The two addresses are buffer_reference types rather than uint64_t so this compiles without the
// shaderInt64 device feature, which the renderer does not enable.
struct GeometryRecord {
    Vertices vertices;
    Indices  indices;
    uint     textureIndex;
    uint     alphaTested;
    // The authored alpha test, packed by Vk::packMaterialBits. Read by anyhit.rahit; declared here
    // only so the two mirrors of this struct stay identical.
    uint     alphaBits;
    uint     pad1;
};

layout(set = 0, binding = 6, std430) readonly buffer Geometries { GeometryRecord records[]; } geometries;

// Slot 0 is the 1x1 white fallback, so scene texture i lives at i + 1. Unlike gbuffer.frag, where the
// index comes from a push constant, textureIndex here varies per hit within a subgroup and needs
// nonuniformEXT at every use.
layout(set = 0, binding = 7) uniform sampler2D textures[1024];

// Interleaved layout from vkgeometry.hpp: 12 floats per vertex, texcoord at floats 6 and 7.
const uint floatsPerVertex = 12;
const uint texCoordOffset = 6;

// Duplicated verbatim in anyhit.rahit. CompileShaders.cmake runs glslangValidator without an
// include path, so a shared .glsl is not an option; edit both copies together.
vec2 vertexTexCoord(Vertices vertices, uint index) {
    uint base = index * floatsPerVertex + texCoordOffset;
    return vec2(vertices.v[base], vertices.v[base + 1]);
}

vec2 interpolateTexCoord(Vertices vertices, Indices indices, vec3 weights) {
    uint triangle = uint(gl_PrimitiveID) * 3;
    return vertexTexCoord(vertices, indices.i[triangle + 0]) * weights.x
        + vertexTexCoord(vertices, indices.i[triangle + 1]) * weights.y
        + vertexTexCoord(vertices, indices.i[triangle + 2]) * weights.z;
}

void main() {
    GeometryRecord record = geometries.records[gl_InstanceCustomIndexEXT];

    vec3 weights = vec3(1.0 - attribs.x - attribs.y, attribs.x, attribs.y);
    vec2 uv = interpolateTexCoord(record.vertices, record.indices, weights);
    vec3 albedo = texture(textures[nonuniformEXT(record.textureIndex)], uv).rgb;

    // w carries the hit distance because raygen distinguishes hit from miss on w > 0; miss.rmiss
    // writes -1 there.
    payload = vec4(albedo, gl_HitTEXT);
}
