#ifdef OPENMW_USE_VULKAN

#include "vklightcollector.hpp"

#include <cmath>

#include <osg/Vec4f>

#include <components/sceneutil/lightmanager.hpp>

namespace
{
    /// The exact sRGB EOTF, the same one VkRenderingManager decodes its sun and ambient with. Morrowind's
    /// light colours were authored against gamma-space compositing and the OSG path still lights in gamma
    /// space; the Vulkan renderer lights in linear, so every colour has to be decoded on the way in or the
    /// whole scene is systematically too bright.
    ///
    /// Radii, attenuation coefficients and distances are not colours and must never go through this.
    ///
    /// Negative lights (the ESM Negative flag) arrive with a negated diffuse - see
    /// SceneUtil::createLightSource in components/sceneutil/lightutil.cpp, which does `diffuse *= -1` -
    /// and std::pow of a negative base is NaN. So decode the magnitude and put the sign back, which keeps
    /// the subtractive light subtractive by the same amount it would be additive.
    float srgbToLinear(float c)
    {
        const float a = std::abs(c);
        const float linear = a <= 0.04045f ? a / 12.92f : std::pow((a + 0.055f) / 1.055f, 2.4f);
        return c < 0.f ? -linear : linear;
    }

    void writeColor(float (&out)[3], const osg::Vec4f& color)
    {
        out[0] = srgbToLinear(color.r());
        out[1] = srgbToLinear(color.g());
        out[2] = srgbToLinear(color.b());
    }
}

namespace MWRender
{
    VkLightCollector::VkLightCollector(const SceneUtil::LightManager* lightManager)
        : mLightManager(lightManager)
    {
    }

    const std::vector<VkPointLight>& VkLightCollector::collect(
        std::size_t frameNumber, float maxDistance, const osg::Vec3f& viewerPos)
    {
        mLights.clear();

        if (!mLightManager)
            return mLights;

        // Matches the OSG path, which scales every radius by this before culling or lighting with it -
        // see LightManager::getLightListStateSet and LightManagerCullCallback::operator().
        const float radiusMultiplier = mLightManager->getPointLightRadiusMultiplier();

        // LightManager::getLightsInViewSpace() fades a light's diffuse and specular towards zero over the
        // last stretch before this same distance, and it has already run for the main camera by the time
        // we get here, so that fade is baked into the colours we read. All that is left for us is the
        // hard cull, on distance to the light's centre, which is the quantity that fade is driven by.
        const float maxDistance2 = maxDistance * maxDistance;

        for (const SceneUtil::LightManager::LightSourceTransform& transform : mLightManager->getLights())
        {
            SceneUtil::LightSource* lightSource = transform.mLightSource;
            if (!lightSource)
                continue;

            const float radius = lightSource->getRadius() * radiusMultiplier;
            if (radius <= 0.f)
                continue;

            // The world matrix the update traversal found this light at. LightManager::addLight() writes
            // exactly this translation into the Light's position, so Light::getPosition() is world space
            // too at the point we read it - nothing between there and here overwrites it, the view space
            // versions the OSG path uses are all built into temporaries. Taking it from the matrix is
            // simply the one that cannot be mistaken for view space.
            const osg::Vec3f position = transform.mWorldMatrix.getTrans();

            if (maxDistance > 0.f && (position - viewerPos).length2() > maxDistance2)
                continue;

            // Read through getLight(frameNumber) rather than caching anything: SceneUtil::LightController
            // writes this frame's flicker and pulse brightness into the Light for frameNumber % 2 during
            // the update traversal, so this is the only way to pick up animated lights at all. A cached
            // Light* would be stuck on whichever of the two buffers it was taken from.
            const SceneUtil::Light* light = lightSource->getLight(frameNumber);

            VkPointLight& out = mLights.emplace_back();

            out.position[0] = position.x();
            out.position[1] = position.y();
            out.position[2] = position.z();
            out.radius = radius;

            writeColor(out.diffuse, light->getDiffuse());
            // Carried inventory lights get an ambient of (1,1,1,1) from ActorAnimation::addHiddenItemLight
            // while world-placed ones get (0,0,0,1) from SceneUtil::addLight, so this term is not always
            // zero and has to travel per light.
            writeColor(out.ambient, light->getAmbient());
            writeColor(out.specular, light->getSpecular());

            // Morrowind's falloff, not physical inverse square. With the stock fallback values in
            // files/openmw.cfg (UseConstant 0, UseLinear 1, LinearMethod 1, LinearValue 3, UseQuadratic 0)
            // SceneUtil::configureLight - components/sceneutil/lightutil.cpp:45 - yields
            // constant 0, linear 3 / radius, quadratic 0. They are read rather than recomputed because a
            // game's INI can change all of it, including turning the quadratic term on outdoors only.
            out.attenuationConstant = light->getConstantAttenuation();
            out.attenuationLinear = light->getLinearAttenuation();
            out.attenuationQuadratic = light->getQuadraticAttenuation();
        }

        return mLights;
    }
}

#endif // OPENMW_USE_VULKAN
