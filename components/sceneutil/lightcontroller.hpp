#ifndef OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H
#define OPENMW_COMPONENTS_SCENEUTIL_LIGHTCONTROLLER_H

#include <components/sceneutil/nodecallback.hpp>
#include <osg/Vec4f>

namespace SceneUtil
{

    class LightSource;

    /// @brief Controller class to handle a pulsing and/or flickering light
    class LightController : public SceneUtil::NodeCallback<LightController, SceneUtil::LightSource*>
    {
    public:
        enum LightType
        {
            LT_Normal,
            LT_Flicker,
            LT_FlickerSlow,
            LT_Pulse,
            LT_PulseSlow
        };

        LightController();

        void setType(LightType type);

        void setDiffuse(const osg::Vec4f& color);
        void setSpecular(const osg::Vec4f& color);

        void operator()(SceneUtil::LightSource* node, osg::NodeVisitor* nv);

        /// The current point of the flicker or pulse cycle, in roughly [0.25, 1].
        ///
        /// This is the scalar the controller multiplies the authored colour by, and until now nothing
        /// outside the controller could see it -- the only evidence of it was the light's diffuse
        /// colour, which cannot be told apart from a light that is simply dimmer. The Vulkan renderer
        /// wants it so a fire's flame can rise and fall with the light the fire casts.
        float getBrightness() const { return mBrightness; }

    private:
        LightType mType;
        osg::Vec4f mDiffuseColor;
        osg::Vec4f mSpecularColor;
        float mPhase;
        float mBrightness;
        double mStartTime;
        double mLastTime;
        float mTicksToAdvance;
    };

}

#endif
