#ifndef OPENMW_COMPONENTS_RENDER_SCENE_H
#define OPENMW_COMPONENTS_RENDER_SCENE_H

#include <cmath>

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

    inline bool valid(float value)
    {
        return std::isfinite(value);
    }

    inline bool valid(const Vec3& value)
    {
        return valid(value.x) && valid(value.y) && valid(value.z);
    }

    inline bool valid(const Vec4& value)
    {
        return valid(value.x) && valid(value.y) && valid(value.z) && valid(value.w);
    }

    inline bool valid(const Quat& value)
    {
        return valid(value.x) && valid(value.y) && valid(value.z) && valid(value.w);
    }

    inline bool valid(const Mat4& value)
    {
        for (const float entry : value.data)
        {
            if (!valid(entry))
                return false;
        }
        return true;
    }

    // Renderer-neutral per-frame data shared by backend adapters.
    struct SceneData
    {
        Mat4 view;
        Mat4 projection;
        Mat4 viewInverse;
        Mat4 projInverse;
        Vec4 sunDirection;
        Vec4 sunColor;
        Vec4 ambientColor;
        Vec4 fogColor;
        // x = fog start distance, y = fog end distance. A non-positive range
        // disables fog for fixtures that do not provide atmospheric state.
        Vec4 fogParameters;

        bool valid() const
        {
            return Render::valid(view) && Render::valid(projection) && Render::valid(viewInverse)
                && Render::valid(projInverse) && Render::valid(sunDirection) && Render::valid(sunColor)
                && Render::valid(ambientColor) && Render::valid(fogColor) && Render::valid(fogParameters);
        }
    };
}

#endif
