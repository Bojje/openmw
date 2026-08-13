#ifndef OPENMW_COMPONENTS_RENDER_SCENE_H
#define OPENMW_COMPONENTS_RENDER_SCENE_H

#include <array>
#include <cmath>
#include <functional>
#include <string_view>

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

    constexpr Mat4 identityMat4()
    {
        return { { 1.f, 0.f, 0.f, 0.f,
            0.f, 1.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, 0.f, 0.f, 1.f } };
    }

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
        static constexpr std::size_t maxPointLights = 16;

        Mat4 view = identityMat4();
        Mat4 projection = identityMat4();
        Mat4 viewInverse = identityMat4();
        Mat4 projInverse = identityMat4();
        Vec4 sunDirection{ 0.f, 0.f, -1.f, 0.f };
        Vec4 sunColor{ 1.f, 1.f, 1.f, 1.f };
        Vec4 ambientColor{ 0.f, 0.f, 0.f, 1.f };
        Vec4 fogColor{ 0.f, 0.f, 0.f, 1.f };
        // x = fog start distance, y = fog end distance. A non-positive range
        // disables fog for fixtures that do not provide atmospheric state.
        Vec4 fogParameters{ 0.f, 0.f, 0.f, 0.f };
        // Weather-provided horizon color for pixels that do not contain world
        // geometry. The Vulkan composite derives its sky gradient from this
        // value instead of keeping a backend-local hard-coded sky.
        Vec4 skyColor{ 0.6f, 0.75f, 0.9f, 1.f };
        // Renderer-neutral effect clock. The Vulkan composite uses x for
        // procedural water motion and y as the underwater camera flag; the
        // remaining components are reserved.
        Vec4 effectTime{ 0.f, 0.f, 0.f, 0.f };
        // Bounded renderer-neutral point lights. Positions use w=1 and the
        // matching color/radius entries use w for the attenuation radius.
        std::array<Vec4, maxPointLights> pointLightPositions{};
        std::array<Vec4, maxPointLights> pointLightColorsAndRadii{};
        // x is the number of active entries; the remaining components are
        // reserved for future light metadata.
        Vec4 pointLightCount{ 0.f, 0.f, 0.f, 0.f };

        bool valid() const
        {
            return Render::valid(view) && Render::valid(projection) && Render::valid(viewInverse)
                && Render::valid(projInverse) && Render::valid(sunDirection) && Render::valid(sunColor)
                && Render::valid(ambientColor) && Render::valid(fogColor) && Render::valid(fogParameters)
                && Render::valid(skyColor) && Render::valid(effectTime) && Render::valid(pointLightCount)
                && pointLightCount.x >= 0.f && pointLightCount.x <= static_cast<float>(maxPointLights)
                && std::floor(pointLightCount.x) == pointLightCount.x
                && [&] {
                       const std::size_t count = static_cast<std::size_t>(pointLightCount.x);
                       for (std::size_t i = 0; i < count; ++i)
                       {
                           if (!Render::valid(pointLightPositions[i])
                               || !Render::valid(pointLightColorsAndRadii[i])
                               || pointLightColorsAndRadii[i].x < 0.f || pointLightColorsAndRadii[i].y < 0.f
                               || pointLightColorsAndRadii[i].z < 0.f
                               || pointLightColorsAndRadii[i].w <= 0.f)
                               return false;
                       }
                       return true;
                   }();
        }
    };

    using SceneSynchronizer = std::function<void(SceneData&)>;
}

#endif
