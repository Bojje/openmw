#version 460

// Expands one particle into one camera-facing quad. No vertex buffer and no index buffer: the corner
// comes from gl_VertexIndex and the particle from gl_InstanceIndex, so the whole effect system costs
// one vkCmdDraw and one storage buffer write per frame.

struct ParticleQuad {
    vec3 position;   // world space, already carrying the emitter's transform
    float size;      // world-unit half extent
    vec4 colour;     // straight from the simulation, alpha included
    uvec4 params;    // .x = slot in the sampler array, .y = authored to blend additively
};

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 projection;
    mat4 viewInverse;
    mat4 projInverse;
    vec4 sunDirection;
    vec4 sunColor;
} camera;

layout(set = 0, binding = 3, std430) readonly buffer Particles {
    ParticleQuad quads[];
} particles;

layout(location = 0) out vec2 fragUv;
layout(location = 1) out vec4 fragColour;
layout(location = 2) flat out uint fragTexture;
layout(location = 3) flat out uint fragAdditive;

void main() {
    ParticleQuad quad = particles.quads[gl_InstanceIndex];

    // Two triangles, six vertices, corners in the order (0,0) (1,0) (1,1) (0,0) (1,1) (0,1).
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );
    vec2 corner = corners[gl_VertexIndex];
    fragUv = corner;

    // Billboarded against the camera rather than against any world axis. The view matrix's rows are
    // the camera's basis vectors in world space, so its first two columns transposed give right and
    // up without inverting anything.
    vec3 right = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 up = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    vec2 offset = (corner - 0.5) * 2.0 * quad.size;
    vec3 world = quad.position + right * offset.x + up * offset.y;

    fragColour = quad.colour;
    fragTexture = quad.params.x;
    fragAdditive = quad.params.y;
    gl_Position = camera.projection * camera.view * vec4(world, 1.0);
}
