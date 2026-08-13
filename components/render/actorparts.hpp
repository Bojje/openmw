#ifndef OPENMW_COMPONENTS_RENDER_ACTORPARTS_H
#define OPENMW_COMPONENTS_RENDER_ACTORPARTS_H

#include <algorithm>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include <components/esm3/loadarmo.hpp>
#include <components/esm3/loadbody.hpp>

namespace Render
{
    // Select the same race/gender/view body-part fallback set used by the
    // reference actor renderer, without requiring an OSG animation owner.
    inline const std::vector<const ESM::BodyPart*>& selectNpcBodyParts(const ESM::RefId& race, bool female,
        bool firstPerson, bool werewolf, std::span<const ESM::BodyPart* const> available)
    {
        constexpr int flagFirstPerson = 1 << 1;
        constexpr int flagFemale = 1 << 0;
        int flags = werewolf ? -1 : 0;
        if (female)
            flags |= flagFemale;
        if (firstPerson)
            flags |= flagFirstPerson;

        using Cache = std::map<std::pair<ESM::RefId, int>, std::vector<const ESM::BodyPart*>>;
        static Cache cache;
        auto found = cache.find({ race, flags });
        if (found != cache.end())
            return found->second;

        auto& parts = cache[{ race, flags }];
        parts.resize(ESM::PRT_Count, nullptr);
        if (werewolf)
            return parts;

        using BodyPartMap = std::multimap<ESM::BodyPart::MeshPart, ESM::PartReferenceType>;
        static const BodyPartMap bodyPartMap = { { ESM::BodyPart::MP_Neck, ESM::PRT_Neck },
            { ESM::BodyPart::MP_Chest, ESM::PRT_Cuirass }, { ESM::BodyPart::MP_Groin, ESM::PRT_Groin },
            { ESM::BodyPart::MP_Hand, ESM::PRT_RHand }, { ESM::BodyPart::MP_Hand, ESM::PRT_LHand },
            { ESM::BodyPart::MP_Wrist, ESM::PRT_RWrist }, { ESM::BodyPart::MP_Wrist, ESM::PRT_LWrist },
            { ESM::BodyPart::MP_Forearm, ESM::PRT_RForearm }, { ESM::BodyPart::MP_Forearm, ESM::PRT_LForearm },
            { ESM::BodyPart::MP_Upperarm, ESM::PRT_RUpperarm }, { ESM::BodyPart::MP_Upperarm, ESM::PRT_LUpperarm },
            { ESM::BodyPart::MP_Foot, ESM::PRT_RFoot }, { ESM::BodyPart::MP_Foot, ESM::PRT_LFoot },
            { ESM::BodyPart::MP_Ankle, ESM::PRT_RAnkle }, { ESM::BodyPart::MP_Ankle, ESM::PRT_LAnkle },
            { ESM::BodyPart::MP_Knee, ESM::PRT_RKnee }, { ESM::BodyPart::MP_Knee, ESM::PRT_LKnee },
            { ESM::BodyPart::MP_Upperleg, ESM::PRT_RLeg }, { ESM::BodyPart::MP_Upperleg, ESM::PRT_LLeg },
            { ESM::BodyPart::MP_Tail, ESM::PRT_Tail } };

        for (const ESM::BodyPart* bodypart : available)
        {
            if (bodypart == nullptr || bodypart->mData.mFlags & ESM::BodyPart::BPF_NotPlayable
                || bodypart->mData.mType != ESM::BodyPart::MT_Skin || bodypart->mRace != race)
                continue;

            const bool partFirstPerson = ESM::isFirstPersonBodyPart(*bodypart);
            const bool isHand = bodypart->mData.mPart == ESM::BodyPart::MP_Hand
                || bodypart->mData.mPart == ESM::BodyPart::MP_Wrist
                || bodypart->mData.mPart == ESM::BodyPart::MP_Forearm
                || bodypart->mData.mPart == ESM::BodyPart::MP_Upperarm;
            const bool sameGender = static_cast<bool>(bodypart->mData.mFlags & ESM::BodyPart::BPF_Female) == female;
            const auto assignParts = [&](auto&& assign) {
                auto it = bodyPartMap.lower_bound(static_cast<ESM::BodyPart::MeshPart>(bodypart->mData.mPart));
                while (it != bodyPartMap.end() && it->first == bodypart->mData.mPart)
                {
                    assign(it->second);
                    ++it;
                }
            };

            if (firstPerson && isHand && !partFirstPerson)
            {
                assignParts([&](ESM::PartReferenceType part) {
                    if ((!parts[part] && sameGender) || (sameGender && parts[part]
                            && static_cast<bool>(parts[part]->mData.mFlags & ESM::BodyPart::BPF_Female) != female)
                        || (!parts[part] && female))
                        parts[part] = bodypart;
                });
                continue;
            }
            if (partFirstPerson != firstPerson)
                continue;
            if (female && !sameGender)
            {
                assignParts([&](ESM::PartReferenceType part) {
                    if (!parts[part] || (isHand && !ESM::isFirstPersonBodyPart(*parts[part]) && partFirstPerson))
                        parts[part] = bodypart;
                });
                continue;
            }
            if (!sameGender)
                continue;
            assignParts([&](ESM::PartReferenceType part) { parts[part] = bodypart; });
        }
        return parts;
    }
}

#endif
