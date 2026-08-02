#ifndef OPENMW_MWRENDER_VKLIGHTCOLLECTOR_H
#define OPENMW_MWRENDER_VKLIGHTCOLLECTOR_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <vector>

#include <osg/Vec3f>

namespace SceneUtil
{
    class LightManager;
}

namespace MWRender
{
    /// One of Morrowind's point lights - a torch, a candle, a brazier, a spell effect - in the form the
    /// Vulkan lighting pass consumes.
    ///
    /// Four vec4s, 64 bytes, no padding needed: in std430 a struct of four vec4s has a base alignment
    /// of 16 and an array stride of 64, so a std::vector<VkPointLight> can be memcpy'd straight into an
    /// SSBO. The alignas(16) and the static_asserts below exist to keep it that way. The scalar in each
    /// vec4's w slot is what fills the hole a bare vec3 would leave.
    ///
    /// Byte offsets:
    ///   0 position.x   4 position.y   8 position.z  12 radius
    ///  16 diffuse.r   20 diffuse.g   24 diffuse.b   28 attenuationConstant
    ///  32 ambient.r   36 ambient.g   40 ambient.b   44 attenuationLinear
    ///  48 specular.r  52 specular.g  56 specular.b  60 attenuationQuadratic
    struct alignas(16) VkPointLight
    {
        /// World space, not view space, and in world units. Not a colour - never gamma decoded.
        float position[3];
        /// Cutoff distance in world units, already multiplied by the light manager's radius
        /// multiplier (the "light bounds multiplier" setting) the way the OSG path does.
        float radius;
        /// Linear, decoded from the gamma-space colour the OSG lighting path carries. May be negative
        /// component-wise for Morrowind's negative lights, which subtract light instead of adding it.
        float diffuse[3];
        float attenuationConstant;
        /// Linear. Zero for world-placed lights but (1,1,1) for a light carried in an actor's
        /// inventory, and it is applied with no N.L term, which is what makes torch-lit rooms soft
        /// rather than spotlit. See calcPointLighting in files/shaders/lib/light/util.glsl.
        float ambient[3];
        float attenuationLinear;
        /// Linear. Zeroed for negative lights upstream, in SceneUtil::createLightSource.
        float specular[3];
        float attenuationQuadratic;
    };

    static_assert(sizeof(VkPointLight) == 64, "VkPointLight must match a std430 array stride of 64");
    static_assert(alignof(VkPointLight) == 16, "VkPointLight must match a std430 base alignment of 16");

    /// Flattens SceneUtil::LightManager's per-frame light list into an array a GPU buffer can be filled
    /// from. The manager collects those lights during the OSG update traversal and clears them at the
    /// start of the next frame, so collect() must be called in between - i.e. after
    /// osgViewer::Viewer::renderingTraversals(), which is where the Vulkan renderer runs.
    ///
    /// Holds no ownership: the light manager outlives the renderer, being part of the scene graph.
    class VkLightCollector
    {
    public:
        explicit VkLightCollector(const SceneUtil::LightManager* lightManager);

        /// Rebuilds and returns the light array.
        ///
        /// \a frameNumber must be the same number the update traversal ran with, i.e.
        /// osgViewer::Viewer::getFrameStamp()->getFrameNumber(). SceneUtil::Light is double buffered on
        /// frame % 2 and the animated lights are written into the current frame's copy, so reading with
        /// the wrong number silently yields last frame's flicker.
        ///
        /// \a maxDistance culls lights whose centre is further than that from \a viewerPos, in world
        /// units; pass LightManager::getPointLightFadeEnd(), or <= 0 for no distance limit.
        const std::vector<VkPointLight>& collect(
            std::size_t frameNumber, float maxDistance, const osg::Vec3f& viewerPos);

        /// The result of the last collect(), without recollecting.
        const std::vector<VkPointLight>& getLights() const { return mLights; }

    private:
        const SceneUtil::LightManager* mLightManager;

        /// Cleared but never shrunk between frames: the light count is roughly stable from one frame to
        /// the next, so after a few frames this stops allocating entirely.
        std::vector<VkPointLight> mLights;
    };
}

#endif // OPENMW_USE_VULKAN
#endif
