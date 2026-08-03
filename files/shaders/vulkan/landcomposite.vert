#version 460

// One full-target triangle for the land texture composite bake. No vertex buffer: the position and
// the UV are derived from gl_VertexIndex, the same trick composite.vert uses, so the bake needs no
// geometry upload and no vertex input state at all.
//
// A triangle rather than two triangles for a quad. It costs a little overdraw outside the target and
// saves the diagonal seam a quad has, where the two triangles' interpolation meets.

layout(location = 0) out vec2 fragUv;

void main() {
    // (0,0), (2,0), (0,2) in UV, which is (-1,-1), (3,-1), (-1,3) in clip space.
    fragUv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(fragUv * 2.0 - 1.0, 0.0, 1.0);
}
