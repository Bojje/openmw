#version 460

// The water surface: one flat grid centred on the camera, drawn as a forward blended pass inside the
// composite render pass.
//
// It used to be one opaque quad per cell submitted into the G-buffer, and that had two problems, only
// one of which is the obvious one. The obvious one is that the sea visibly stopped at the edge of the
// loaded grid. The other is that a water pixel's depth was then the *water surface*, so the seabed was
// not in the depth buffer at all and every depth-based effect -- the whole reason water looks like
// water -- had nothing to read.
//
// No vertex buffer and no index buffer: the grid comes out of gl_VertexIndex, the same way the
// particle billboards get their corners. There is nothing per-vertex worth storing. Every vertex is a
// lattice point on a horizontal plane whose height and centre both come from the scene uniform, so a
// buffer would hold numbers this shader computes for free and would have to be re-uploaded every time
// the player moved or walked into a cell with a different water level.

layout(set = 0, binding = 0) uniform SceneUBO {
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
    // .x is the sun's angular radius, which raygen owns. The other three were the documented spare of
    // this vec4 and are now .y = seconds, .z = the water plane's world Z, .w = 1 when there is a water
    // plane at all. They went here rather than into a new field because SceneData's offsets past
    // denoiseParams are pinned by static_asserts and mirrored by hand in three shaders.
    vec4 sunParams;
    uint frameIndex;
    uint lightCount;
    uint isInterior;
    uint waterNormalMap;
} scene;

// 150 cells across, subdivided 40 ways, which is what MWRender::Water asks for at water.cpp:447 --
// createWaterGeometry(Constants::CellSizeInUnits * 150, 40, 900).
//
// The subdivision is not decoration and it is not for shading: the surface is flat and its normal
// comes entirely from the normal map, so one quad would shade identically. It is there because
// components/sceneutil/waterutil.cpp subdivides for the reason its comment gives -- "some drivers
// don't like huge triangles" -- and this sheet is 1.2 million units on a side. sSegments must agree
// with sWaterVertexCount in vkrenderer.hpp; disagreeing draws part of the grid or runs off the end of
// it, and neither looks like a mismatched constant.
const float sExtent = 8192.0 * 150.0;
const int sSegments = 40;

void main() {
    int quad = gl_VertexIndex / 6;
    int corner = gl_VertexIndex % 6;

    // Corners (0,0) (1,0) (1,1) (0,0) (1,1) (0,1), as the particle quad has them. The winding is not
    // load-bearing: the pipeline culls nothing, because the player swims and the underside of the sea
    // has to rasterise too. That matches the OSG renderer, which turns culling off outright at
    // water.cpp:627.
    const vec2 corners[6] = vec2[6](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );

    vec2 cell = (vec2(float(quad % sSegments), float(quad / sSegments)) + corners[corner])
        / float(sSegments);
    vec2 local = (cell - 0.5) * sExtent;

    // Centred on the camera, not on a cell. The view matrix's inverse has the camera position in its
    // translation column, so this needs nothing pushed.
    //
    // Moving the grid with the camera does not move the waves: the fragment shader recovers its world
    // position by intersecting the view ray with the plane rather than by interpolating one across
    // these vertices, so where the grid happens to sit has no effect on what is drawn on it.
    vec3 camera = scene.viewInverse[3].xyz;
    vec3 world = vec3(camera.xy + local, scene.sunParams.z);

    gl_Position = scene.projection * scene.view * vec4(world, 1.0);
}
