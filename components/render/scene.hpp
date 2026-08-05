#ifndef OPENMW_COMPONENTS_RENDER_SCENE_H
#define OPENMW_COMPONENTS_RENDER_SCENE_H

namespace Render
{
    struct Vec3
    {
        float x, y, z;
    };

    struct Mat4
    {
        float data[16];
    };

    struct Vec4
    {
        float x, y, z, w;
    };

    struct Quat
    {
        float x, y, z, w;
    };

    // Renderer-neutral per-frame data shared by backend adapters.
    struct SceneData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewInverse;
        Mat4 projInverse;
        Vec4 sunDirection;
        Vec4 sunColor;
    };
}

#endif
