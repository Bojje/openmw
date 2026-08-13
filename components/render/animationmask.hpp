#ifndef OPENMW_COMPONENTS_RENDER_ANIMATIONMASK_H
#define OPENMW_COMPONENTS_RENDER_ANIMATIONMASK_H

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace Render
{
    enum class AnimationBoneGroup
    {
        LowerBody,
        Torso,
        LeftArm,
        RightArm,
    };

    inline constexpr unsigned AnimationMask_LowerBody = 1u << 0;
    inline constexpr unsigned AnimationMask_Torso = 1u << 1;
    inline constexpr unsigned AnimationMask_LeftArm = 1u << 2;
    inline constexpr unsigned AnimationMask_RightArm = 1u << 3;

    inline AnimationBoneGroup classifyAnimationBone(std::string_view name)
    {
        std::string normalized(name);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });

        static constexpr std::array<std::string_view, 8> torso = { "bip01 spine1", "bip01 spine2", "bip01 neck",
            "bip01 head", "head", "neck", "chest", "groin" };
        if (std::find(torso.begin(), torso.end(), normalized) != torso.end())
            return AnimationBoneGroup::Torso;

        static constexpr std::array<std::string_view, 25> leftArm = { "bip01 l clavicle", "left clavicle",
            "bip01 l upperarm", "left upper arm", "bip01 l forearm", "left forearm", "bip01 l hand", "left hand",
            "left wrist", "shield bone", "bip01 l pinky1", "bip01 l pinky2", "bip01 l pinky3", "bip01 l ring1",
            "bip01 l ring2", "bip01 l ring3", "bip01 l middle1", "bip01 l middle2", "bip01 l middle3",
            "bip01 l pointer1", "bip01 l pointer2", "bip01 l pointer3", "bip01 l thumb1", "bip01 l thumb2",
            "bip01 l thumb3" };
        if (std::find(leftArm.begin(), leftArm.end(), normalized) != leftArm.end())
            return AnimationBoneGroup::LeftArm;

        static constexpr std::array<std::string_view, 25> rightArm = { "bip01 r clavicle", "right clavicle",
            "bip01 r upperarm", "right upper arm", "bip01 r forearm", "right forearm", "bip01 r hand", "right hand",
            "right wrist", "weapon bone", "bip01 r pinky1", "bip01 r pinky2", "bip01 r pinky3", "bip01 r ring1",
            "bip01 r ring2", "bip01 r ring3", "bip01 r middle1", "bip01 r middle2", "bip01 r middle3",
            "bip01 r pointer1", "bip01 r pointer2", "bip01 r pointer3", "bip01 r thumb1", "bip01 r thumb2",
            "bip01 r thumb3" };
        if (std::find(rightArm.begin(), rightArm.end(), normalized) != rightArm.end())
            return AnimationBoneGroup::RightArm;

        return AnimationBoneGroup::LowerBody;
    }

    inline unsigned animationMaskForBone(AnimationBoneGroup group)
    {
        switch (group)
        {
            case AnimationBoneGroup::Torso: return AnimationMask_Torso;
            case AnimationBoneGroup::LeftArm: return AnimationMask_LeftArm;
            case AnimationBoneGroup::RightArm: return AnimationMask_RightArm;
            case AnimationBoneGroup::LowerBody: return AnimationMask_LowerBody;
        }
        return AnimationMask_LowerBody;
    }

    inline bool animationBoneInMask(std::string_view name, unsigned mask)
    {
        return (animationMaskForBone(classifyAnimationBone(name)) & mask) != 0;
    }
}

#endif
