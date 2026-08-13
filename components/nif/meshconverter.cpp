#include "meshconverter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <stdexcept>
#include <utility>

#include "data.hpp"
#include "controller.hpp"
#include "extra.hpp"
#include "nifkey.hpp"
#include "node.hpp"
#include "particle.hpp"
#include "property.hpp"
#include "texture.hpp"
#include <components/render/meshconversion.hpp>
#include <components/render/math.hpp>
#include <components/render/animationmask.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/misc/strings/lower.hpp>

namespace Nif
{
    namespace
    {
        Render::Mat4 toRenderMatrix(const NiTransform& transform);
        Render::Mat4 toRenderMatrix(const NiQuatTransform& transform);

        Matrix3 toMatrix3(const osg::Quat& rotation)
        {
            const osg::Matrixf matrix(rotation);
            Matrix3 result;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    result.mValues[i][j] = matrix(j, i);
            return result;
        }

        float extrapolateControllerTime(float time, float start, float stop, NiTimeController::ExtrapolationMode mode)
        {
            if (!std::isfinite(time) || !std::isfinite(start) || !std::isfinite(stop) || stop <= start)
                return start;
            if (time >= start && time <= stop)
                return time;

            const float delta = stop - start;
            switch (mode)
            {
                case NiTimeController::ExtrapolationMode::Cycle:
                {
                    const float cycles = (time - start) / delta;
                    return start + (cycles - std::floor(cycles)) * delta;
                }
                case NiTimeController::ExtrapolationMode::Reverse:
                {
                    const float cycles = (time - start) / delta;
                    const float remainder = (cycles - std::floor(cycles)) * delta;
                    return (static_cast<int>(std::fabs(std::floor(cycles))) % 2) == 0
                        ? start + remainder
                        : stop - remainder;
                }
                case NiTimeController::ExtrapolationMode::Constant:
                default:
                    return std::clamp(time, start, stop);
            }
        }

        float controllerTime(const NiTimeController& controller, float value)
        {
            return extrapolateControllerTime(controller.mFrequency * value + controller.mPhase,
                controller.mTimeStart, controller.mTimeStop, controller.extrapolationMode());
        }

        float controllerSequenceTime(const NiControllerSequence& sequence, float value)
        {
            const float time = sequence.mFrequency * value + sequence.mPhase;
            const float start = sequence.mStartTime;
            const float stop = sequence.mStopTime;
            const float directedTime = sequence.mPlayBackwards ? start + stop - time : time;
            return extrapolateControllerTime(directedTime, start, stop, sequence.mExtrapolationMode);
        }

        std::string normalizedBoneName(std::string_view name)
        {
            return Render::normalizeAnimationBoneName(name);
        }

        template <class Map, class Interpolate>
        typename Map::ValueType sampleKeys(const std::shared_ptr<Map>& map, float time,
            typename Map::ValueType defaultValue, Interpolate&& interpolate)
        {
            if (!map || map->mKeys.empty())
                return defaultValue;
            if (time <= map->mKeys.front().first)
                return map->mKeys.front().second.mValue;

            const auto high = std::upper_bound(map->mKeys.begin(), map->mKeys.end(), time,
                [](float value, const auto& key) { return value < key.first; });
            if (high == map->mKeys.end())
                return map->mKeys.back().second.mValue;
            const auto low = std::prev(high);
            if (high->first == low->first)
                return low->second.mValue;
            const float fraction = (time - low->first) / (high->first - low->first);
            return interpolate(low->second, high->second, fraction, high->first - low->first,
                map->mInterpolationType);
        }

        float interpolateFloat(const KeyT<float>& lhs, const KeyT<float>& rhs, float fraction, float duration,
            unsigned int type)
        {
            if (type == InterpolationType_Constant)
                return lhs.mValue;
            if (type == InterpolationType_Quadratic)
            {
                const float fraction2 = fraction * fraction;
                const float fraction3 = fraction2 * fraction;
                return (2.f * fraction3 - 3.f * fraction2 + 1.f) * lhs.mValue
                    + (fraction3 - 2.f * fraction2 + fraction) * duration * lhs.mOutTan
                    + (-2.f * fraction3 + 3.f * fraction2) * rhs.mValue
                    + (fraction3 - fraction2) * duration * rhs.mInTan;
            }
            return lhs.mValue + (rhs.mValue - lhs.mValue) * fraction;
        }

        osg::Vec3f interpolateVector(const KeyT<osg::Vec3f>& lhs, const KeyT<osg::Vec3f>& rhs,
            float fraction, float duration, unsigned int type)
        {
            if (type == InterpolationType_Constant)
                return lhs.mValue;
            if (type == InterpolationType_Quadratic)
            {
                const float fraction2 = fraction * fraction;
                const float fraction3 = fraction2 * fraction;
                return lhs.mValue * (2.f * fraction3 - 3.f * fraction2 + 1.f)
                    + lhs.mOutTan * ((fraction3 - 2.f * fraction2 + fraction) * duration)
                    + rhs.mValue * (-2.f * fraction3 + 3.f * fraction2)
                    + rhs.mInTan * ((fraction3 - fraction2) * duration);
            }
            return lhs.mValue + (rhs.mValue - lhs.mValue) * fraction;
        }

        osg::Quat interpolateQuaternion(const KeyT<osg::Quat>& lhs, const KeyT<osg::Quat>& rhs,
            float fraction, float, unsigned int type)
        {
            if (type == InterpolationType_Constant)
                return lhs.mValue;
            osg::Quat result;
            result.slerp(fraction, lhs.mValue, rhs.mValue);
            return result;
        }

        struct SampledNodeTransform
        {
            NiTransform value;
        };

        SampledNodeTransform sampleKeyframeData(const NiKeyframeData* data, float sampleTime,
            Matrix3 defaultRotationMatrix, osg::Quat defaultRotation, osg::Vec3f defaultTranslation, float defaultScale)
        {
            SampledNodeTransform result{ NiTransform::getIdentity() };
            result.value.mRotation = defaultRotationMatrix;
            result.value.mTranslation = defaultTranslation;
            result.value.mScale = defaultScale;
            if (data == nullptr)
                return result;

            if (data->mRotations && !data->mRotations->mKeys.empty())
                result.value.mRotation = toMatrix3(sampleKeys(data->mRotations, sampleTime, defaultRotation,
                    interpolateQuaternion));
            else if ((data->mXRotations && !data->mXRotations->mKeys.empty())
                || (data->mYRotations && !data->mYRotations->mKeys.empty())
                || (data->mZRotations && !data->mZRotations->mKeys.empty()))
            {
                const float x = sampleKeys(data->mXRotations, sampleTime, 0.f, interpolateFloat);
                const float y = sampleKeys(data->mYRotations, sampleTime, 0.f, interpolateFloat);
                const float z = sampleKeys(data->mZRotations, sampleTime, 0.f, interpolateFloat);
                const osg::Quat xr(x, osg::X_AXIS), yr(y, osg::Y_AXIS), zr(z, osg::Z_AXIS);
                switch (data->mAxisOrder)
                {
                    case NiKeyframeData::AxisOrder::Order_XYZ: result.value.mRotation = toMatrix3(xr * yr * zr); break;
                    case NiKeyframeData::AxisOrder::Order_XZY: result.value.mRotation = toMatrix3(xr * zr * yr); break;
                    case NiKeyframeData::AxisOrder::Order_YZX: result.value.mRotation = toMatrix3(yr * zr * xr); break;
                    case NiKeyframeData::AxisOrder::Order_YXZ: result.value.mRotation = toMatrix3(yr * xr * zr); break;
                    case NiKeyframeData::AxisOrder::Order_ZXY: result.value.mRotation = toMatrix3(zr * xr * yr); break;
                    case NiKeyframeData::AxisOrder::Order_ZYX: result.value.mRotation = toMatrix3(zr * yr * xr); break;
                    case NiKeyframeData::AxisOrder::Order_XYX: result.value.mRotation = toMatrix3(xr * yr * xr); break;
                    case NiKeyframeData::AxisOrder::Order_YZY: result.value.mRotation = toMatrix3(yr * zr * yr); break;
                    case NiKeyframeData::AxisOrder::Order_ZXZ: result.value.mRotation = toMatrix3(zr * xr * zr); break;
                }
            }
            if (data->mTranslations && !data->mTranslations->mKeys.empty())
                result.value.mTranslation = sampleKeys(data->mTranslations, sampleTime, defaultTranslation,
                    interpolateVector);
            if (data->mScales && !data->mScales->mKeys.empty())
                result.value.mScale = sampleKeys(data->mScales, sampleTime, defaultScale, interpolateFloat);
            return result;
        }

        SampledNodeTransform sampleKeyframeController(
            const NiKeyframeController& controller, NiTransform defaultTransform, float time)
        {
            SampledNodeTransform result{ defaultTransform };
            const NiKeyframeData* data = nullptr;
            Matrix3 defaultRotationMatrix = result.value.mRotation;
            osg::Quat defaultRotation = result.value.mRotation.toOsgMatrix().getRotate();
            osg::Vec3f defaultTranslation = result.value.mTranslation;
            float defaultScale = result.value.mScale;
            if (!controller.mInterpolator.empty()
                && controller.mInterpolator->mRecordType == RC_NiTransformInterpolator)
            {
                const auto* interpolator
                    = static_cast<const NiTransformInterpolator*>(controller.mInterpolator.getPtr());
                data = interpolator->mData.empty() ? nullptr : interpolator->mData.getPtr();
                defaultRotation = interpolator->mDefaultValue.mRotation;
                defaultRotationMatrix = toMatrix3(defaultRotation);
                defaultTranslation = interpolator->mDefaultValue.mTranslation;
                defaultScale = interpolator->mDefaultValue.mScale;
            }
            else if (!controller.mData.empty())
                data = controller.mData.getPtr();
            return sampleKeyframeData(data, controllerTime(controller, time), defaultRotationMatrix, defaultRotation,
                defaultTranslation, defaultScale);
        }

        std::optional<NiQuatTransform> sampleTransformInterpolator(const NiInterpolator* interpolator, float time);

        std::optional<NiQuatTransform> sampleBlendTransformInterpolator(
            const NiBlendTransformInterpolator& blend, float time)
        {
            struct WeightedTransform
            {
                NiQuatTransform transform;
                float weight;
                int priority;
            };
            std::vector<WeightedTransform> values;
            values.reserve(blend.mItems.size());
            int highestPriority = std::numeric_limits<int>::min();
            for (const NiBlendInterpolator::Item& item : blend.mItems)
            {
                const float weight = std::isfinite(item.mNormalizedWeight) && item.mNormalizedWeight > 0.f
                    ? item.mNormalizedWeight
                    : item.mWeight;
                if (!std::isfinite(weight) || weight <= 0.f)
                    continue;
                const auto value = sampleTransformInterpolator(item.mInterpolator.getPtr(), time);
                if (!value)
                    continue;
                highestPriority = std::max(highestPriority, item.mPriority);
                values.push_back({ *value, weight, item.mPriority });
            }

            if (values.empty() && !blend.mSingleInterpolator.empty())
                return sampleTransformInterpolator(blend.mSingleInterpolator.getPtr(), time);
            if (values.empty())
            {
                return blend.mValue;
            }

            if (highestPriority != std::numeric_limits<int>::min())
                std::erase_if(values, [highestPriority](const WeightedTransform& value) {
                    return value.priority != highestPriority;
                });
            if ((blend.mFlags & NiBlendInterpolator::Flag_OnlyUseHighestWeight) != 0)
            {
                const auto highest = std::max_element(values.begin(), values.end(), [](const auto& lhs, const auto& rhs) {
                    return lhs.weight < rhs.weight;
                });
                return highest->transform;
            }

            float weightSum = 0.f;
            osg::Vec3f translation;
            float scale = 0.f;
            osg::Quat rotation;
            bool first = true;
            for (const WeightedTransform& value : values)
            {
                if (first)
                {
                    rotation = value.transform.mRotation;
                    first = false;
                }
                else
                {
                    const float fraction = value.weight / (weightSum + value.weight);
                    rotation.slerp(fraction, rotation, value.transform.mRotation);
                }
                translation += value.transform.mTranslation * value.weight;
                scale += value.transform.mScale * value.weight;
                weightSum += value.weight;
            }
            if (weightSum <= 0.f)
                return std::nullopt;
            return NiQuatTransform{ translation / weightSum, rotation, scale / weightSum };
        }

        std::optional<NiQuatTransform> sampleTransformInterpolator(const NiInterpolator* interpolator, float time)
        {
            if (interpolator == nullptr || !std::isfinite(time))
                return std::nullopt;
            if (const auto* transform = dynamic_cast<const NiTransformInterpolator*>(interpolator))
            {
                const NiQuatTransform& defaultValue = transform->mDefaultValue;
                const NiKeyframeData* data = transform->mData.empty() ? nullptr : transform->mData.getPtr();
                const SampledNodeTransform sampled = sampleKeyframeData(data, time,
                    toMatrix3(defaultValue.mRotation), defaultValue.mRotation, defaultValue.mTranslation,
                    defaultValue.mScale);
                return NiQuatTransform{ sampled.value.mTranslation,
                    sampled.value.mRotation.toOsgMatrix().getRotate(), sampled.value.mScale };
            }
            if (const auto* blend = dynamic_cast<const NiBlendTransformInterpolator*>(interpolator))
                return sampleBlendTransformInterpolator(*blend, time);
            return std::nullopt;
        }

        SampledNodeTransform sampleNodeTransform(const NiAVObject& node, float time)
        {
            SampledNodeTransform result{ node.mTransform };
            const NiTimeController* controller = node.mController.empty() ? nullptr : node.mController.getPtr();
            while (controller != nullptr)
            {
                if (const auto* keyframe = dynamic_cast<const NiKeyframeController*>(controller))
                    result = sampleKeyframeController(*keyframe, result.value, time);
                controller = controller->mNext.empty() ? nullptr : controller->mNext.getPtr();
            }
            return result;
        }

        void collectBoneTransforms(const NiAVObject& object, const Render::Mat4& parentTransform, float time,
            std::unordered_map<std::string, Render::Mat4>& transforms)
        {
            const Render::Mat4 transform = Render::multiply(parentTransform,
                toRenderMatrix(sampleNodeTransform(object, time).value));
            if (!object.mName.empty())
                // Later animation sources are higher priority in the legacy
                // controller stack, so a later occurrence replaces an older
                // transform for the same bone name.
                transforms.insert_or_assign(normalizedBoneName(object.mName), transform);
            if (const auto* node = dynamic_cast<const NiNode*>(&object))
                for (const auto& child : node->mChildren)
                    if (!child.empty())
                        collectBoneTransforms(*child.getPtr(), transform, time, transforms);
        }

        bool matchesTextKey(std::string_view text, std::string_view requested)
        {
            std::string normalized(text);
            const std::size_t first = normalized.find_first_not_of(" \t\r\n");
            const std::size_t last = normalized.find_last_not_of(" \t\r\n");
            if (first == std::string::npos)
                return false;
            normalized = normalized.substr(first, last - first + 1);
            return Misc::StringUtils::lowerCase(normalized).starts_with(
                Misc::StringUtils::lowerCase(requested));
        }

        std::optional<float> findTextKeyTime(const ExtraList& extras, std::string_view requested)
        {
            for (const ExtraPtr& extra : extras)
            {
                if (extra.empty() || extra->mRecordType != RC_NiTextKeyExtraData)
                    continue;
                const auto* textKeys = static_cast<const NiTextKeyExtraData*>(extra.getPtr());
                for (const NiTextKeyExtraData::TextKey& key : textKeys->mList)
                    if (std::isfinite(key.mTime) && matchesTextKey(key.mText, requested))
                        return key.mTime;
            }
            return std::nullopt;
        }

        std::optional<float> findTextKeyTime(const Record& record, std::string_view requested)
        {
            if (const auto* object = dynamic_cast<const NiObjectNET*>(&record))
            {
                if (const auto time = findTextKeyTime(object->getExtraList(), requested))
                    return time;
            }
            if (const auto* sequence = dynamic_cast<const NiSequence*>(&record))
            {
                if (!sequence->mTextKeys.empty())
                {
                    const ExtraList extras{ sequence->mTextKeys };
                    if (const auto time = findTextKeyTime(extras, requested))
                        return time;
                }
            }
            if (const auto* node = dynamic_cast<const NiNode*>(&record))
            {
                for (const NiAVObjectPtr& child : node->mChildren)
                    if (!child.empty())
                        if (const auto time = findTextKeyTime(*child.getPtr(), requested))
                            return time;
                for (const NiAVObjectPtr& effect : node->mEffects)
                    if (!effect.empty())
                        if (const auto time = findTextKeyTime(*effect.getPtr(), requested))
                            return time;
            }
            return std::nullopt;
        }

        std::string trimTextKey(std::string_view value)
        {
            const std::size_t first = value.find_first_not_of(" \t\r\n");
            if (first == std::string_view::npos)
                return {};
            const std::size_t last = value.find_last_not_of(" \t\r\n");
            return std::string(value.substr(first, last - first + 1));
        }

        void collectTextKeys(const ExtraList& extras, std::string_view group,
            std::vector<Render::AnimationTextKey>& result)
        {
            const std::string prefix = Misc::StringUtils::lowerCase(std::string(group) + ": ");
            for (const ExtraPtr& extra : extras)
            {
                if (extra.empty() || extra->mRecordType != RC_NiTextKeyExtraData)
                    continue;
                const auto* textKeys = static_cast<const NiTextKeyExtraData*>(extra.getPtr());
                for (const NiTextKeyExtraData::TextKey& key : textKeys->mList)
                {
                    if (!std::isfinite(key.mTime))
                        continue;
                    const std::string event = trimTextKey(key.mText);
                    if (Misc::StringUtils::lowerCase(event).starts_with(prefix))
                        result.push_back({ key.mTime, event });
                }
            }
        }

        void collectTextKeys(const Record& record, std::string_view group,
            std::vector<Render::AnimationTextKey>& result)
        {
            if (const auto* object = dynamic_cast<const NiObjectNET*>(&record))
                collectTextKeys(object->getExtraList(), group, result);
            if (const auto* sequence = dynamic_cast<const NiSequence*>(&record))
            {
                if (!sequence->mTextKeys.empty())
                {
                    const ExtraList extras{ sequence->mTextKeys };
                    collectTextKeys(extras, group, result);
                }
            }
            if (const auto* node = dynamic_cast<const NiNode*>(&record))
            {
                for (const NiAVObjectPtr& child : node->mChildren)
                    if (!child.empty())
                        collectTextKeys(*child.getPtr(), group, result);
                for (const NiAVObjectPtr& effect : node->mEffects)
                    if (!effect.empty())
                        collectTextKeys(*effect.getPtr(), group, result);
            }
        }

        struct SequenceTransformState
        {
            NiQuatTransform transform;
            float weight;
        };

        NiQuatTransform blendSequenceTransforms(const SequenceTransformState& lhs, const NiQuatTransform& rhs,
            float rhsWeight)
        {
            const float totalWeight = lhs.weight + rhsWeight;
            if (totalWeight <= 0.f)
                return rhs;
            const float fraction = rhsWeight / totalWeight;
            osg::Quat rotation = lhs.transform.mRotation;
            rotation.slerp(fraction, lhs.transform.mRotation, rhs.mRotation);
            return { (lhs.transform.mTranslation * lhs.weight + rhs.mTranslation * rhsWeight) / totalWeight,
                rotation, (lhs.transform.mScale * lhs.weight + rhs.mScale * rhsWeight) / totalWeight };
        }

        void collectSequenceTransforms(const NiSequenceStreamHelper& sequence, float time, std::string_view group,
            std::string_view startKey, std::string_view stopKey,
            std::unordered_map<std::string, Render::Mat4>& transforms,
            std::unordered_map<std::string, int>& sequencePriorities,
            std::unordered_map<std::string, SequenceTransformState>& weightedSequences)
        {
            const ExtraList extraList = sequence.getExtraList();
            const std::string_view effectiveStartKey = startKey.empty() ? std::string_view("start") : startKey;
            const float segmentStart = group.empty()
                ? 0.f
                : findTextKeyTime(sequence, std::string(group) + ": " + std::string(effectiveStartKey)).value_or(0.f);
            float sampleTime = time + segmentStart;
            if (!group.empty() && !stopKey.empty())
            {
                if (const std::optional<float> stop
                    = findTextKeyTime(sequence, std::string(group) + ": " + std::string(stopKey));
                    stop && *stop >= segmentStart)
                {
                    sampleTime = std::min(sampleTime, *stop);
                }
            }
            const NiTimeController* controller
                = sequence.mController.empty() ? nullptr : sequence.mController.getPtr();
            for (std::size_t i = 1; i < extraList.size() && controller != nullptr;
                 ++i, controller = controller->mNext.empty() ? nullptr : controller->mNext.getPtr())
            {
                const ExtraPtr& extra = extraList[i];
                if (extra.empty() || extra->mRecordType != RC_NiStringExtraData
                    || controller->mRecordType != RC_NiKeyframeController)
                    continue;

                const auto* name = static_cast<const NiStringExtraData*>(extra.getPtr());
                const auto* keyframe = static_cast<const NiKeyframeController*>(controller);
                const NiTransform sampled
                    = sampleKeyframeController(*keyframe, NiTransform::getIdentity(), sampleTime).value;
                const std::string boneName = normalizedBoneName(name->mData);
                const auto priority = sequencePriorities.find(boneName);
                if (priority == sequencePriorities.end() || priority->second <= 0)
                {
                    sequencePriorities.insert_or_assign(boneName, 0);
                    weightedSequences.erase(boneName);
                    transforms.insert_or_assign(boneName, toRenderMatrix(sampled));
                }
            }
        }

        void collectControllerSequenceTransforms(const NiSequence& sequence, float time, std::string_view group,
            std::string_view startKey, std::string_view stopKey,
            std::unordered_map<std::string, Render::Mat4>& transforms,
            std::unordered_map<std::string, int>& sequencePriorities,
            std::unordered_map<std::string, SequenceTransformState>& weightedSequences)
        {
            const auto* controllerSequence = dynamic_cast<const NiControllerSequence*>(&sequence);
            const float sequenceWeight = controllerSequence != nullptr ? controllerSequence->mWeight : 1.f;
            if (!std::isfinite(sequenceWeight) || sequenceWeight <= 0.f)
                return;
            if (!group.empty() && !sequence.mName.empty()
                && Misc::StringUtils::lowerCase(sequence.mName) != Misc::StringUtils::lowerCase(std::string(group)))
                return;

            const std::string_view effectiveStartKey = startKey.empty() ? std::string_view("start") : startKey;
            const float segmentStart = group.empty()
                ? 0.f
                : findTextKeyTime(sequence, std::string(group) + ": " + std::string(effectiveStartKey)).value_or(0.f);
            float sampleTime = time + segmentStart;
            if (!group.empty() && !stopKey.empty())
            {
                if (const std::optional<float> stop
                    = findTextKeyTime(sequence, std::string(group) + ": " + std::string(stopKey));
                    stop && *stop >= segmentStart)
                    sampleTime = std::min(sampleTime, *stop);
            }
            if (controllerSequence != nullptr)
                sampleTime = controllerSequenceTime(*controllerSequence, sampleTime);

            for (const ControlledBlock& block : sequence.mControlledBlocks)
            {
                if (block.mTargetName.empty())
                    continue;
                const std::string targetName = normalizedBoneName(block.mTargetName);
                if (block.mBlendIndexSet
                    && (block.mBlendIndex >= 4
                        || !Render::animationBoneInMask(targetName, 1u << block.mBlendIndex)))
                    continue;
                const float controllerSampleTime = block.mController.empty()
                    ? sampleTime
                    : controllerTime(*block.mController.getPtr(), sampleTime);
                std::optional<NiQuatTransform> sampled;
                if (!block.mBlendInterpolator.empty())
                    sampled = sampleTransformInterpolator(block.mBlendInterpolator.getPtr(), controllerSampleTime);
                if (!sampled && !block.mInterpolator.empty())
                    sampled = sampleTransformInterpolator(block.mInterpolator.getPtr(), controllerSampleTime);
                if (sampled)
                {
                    const int priority = static_cast<int>(block.mPriority);
                    const auto previous = sequencePriorities.find(targetName);
                    if (previous != sequencePriorities.end() && priority < previous->second)
                        continue;

                    if (previous == sequencePriorities.end() || priority > previous->second)
                    {
                        sequencePriorities.insert_or_assign(targetName, priority);
                        if (controllerSequence != nullptr)
                            weightedSequences.insert_or_assign(targetName,
                                SequenceTransformState{ *sampled, sequenceWeight });
                        else
                            weightedSequences.erase(targetName);
                        transforms.insert_or_assign(targetName, toRenderMatrix(*sampled));
                        continue;
                    }

                    const auto weighted = weightedSequences.find(targetName);
                    if (controllerSequence != nullptr && weighted != weightedSequences.end()
                        && (weighted->second.weight != 1.f || sequenceWeight != 1.f))
                    {
                        const NiQuatTransform blended
                            = blendSequenceTransforms(weighted->second, *sampled, sequenceWeight);
                        weighted->second = { blended, weighted->second.weight + sequenceWeight };
                        transforms.insert_or_assign(targetName, toRenderMatrix(blended));
                    }
                    else
                    {
                        weightedSequences.erase(targetName);
                        transforms.insert_or_assign(targetName, toRenderMatrix(*sampled));
                    }
                }
            }
        }

        std::vector<Render::MeshVertexSource> convertVertices(const NiGeometryData& source)
        {
            std::vector<Render::MeshVertexSource> result(source.mVertices.size());

            const bool hasNormals = source.mNormals.size() == source.mVertices.size();
            const bool hasColors = source.mColors.size() == source.mVertices.size();
            const bool hasTexcoords = !source.mUVList.empty()
                && source.mUVList.front().size() == source.mVertices.size();

            for (std::size_t i = 0; i < source.mVertices.size(); ++i)
            {
                const auto& position = source.mVertices[i];
                auto& vertex = result[i];
                vertex.position[0] = position.x();
                vertex.position[1] = position.y();
                vertex.position[2] = position.z();

                if (hasNormals)
                {
                    const auto& normal = source.mNormals[i];
                    vertex.normal[0] = normal.x();
                    vertex.normal[1] = normal.y();
                    vertex.normal[2] = normal.z();
                    vertex.hasNormal = true;
                }

                if (hasTexcoords)
                {
                    const auto& texcoord = source.mUVList.front()[i];
                    vertex.texcoord[0] = texcoord.x();
                    vertex.texcoord[1] = texcoord.y();
                    vertex.hasTexcoord = true;
                }

                if (hasColors)
                {
                    const auto& color = source.mColors[i];
                    vertex.color[0] = color.x();
                    vertex.color[1] = color.y();
                    vertex.color[2] = color.z();
                    vertex.color[3] = color.w();
                    vertex.hasColor = true;
                }
            }

            return result;
        }

        void setShaderTexture(Render::MeshMaterial& material, const BSShaderTextureSetPtr& textureSet,
            bool wrapU, bool wrapV)
        {
            if (textureSet.empty() || textureSet->mTextures.empty())
                return;

            material.albedoTexture = VFS::Path::toNormalized(textureSet->mTextures.front()).value();
            material.albedoWrapU = wrapU;
            material.albedoWrapV = wrapV;
            if (textureSet->mTextures.size() > 1 && !textureSet->mTextures[1].empty())
            {
                material.normalTexture = VFS::Path::toNormalized(textureSet->mTextures[1]).value();
                material.normalMap = !material.normalTexture.empty();
                material.normalWrapU = wrapU;
                material.normalWrapV = wrapV;
            }
            if (textureSet->mTextures.size() > 2 && !textureSet->mTextures[2].empty())
            {
                material.emissiveTexture = VFS::Path::toNormalized(textureSet->mTextures[2]).value();
                material.emissiveWrapU = wrapU;
                material.emissiveWrapV = wrapV;
            }
        }

        Render::MeshMaterial convertMaterial(const NiGeometry& geometry)
        {
            Render::MeshMaterial result;

            for (const auto& property : geometry.mProperties)
            {
                if (property.empty())
                    continue;

                if (const auto* texturing = dynamic_cast<const NiTexturingProperty*>(property.getPtr()))
                {
                    if (texturing->mTextures.size() > NiTexturingProperty::BaseTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::BaseTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.albedoTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.albedoWrapU = texture.wrapS();
                            result.albedoWrapV = texture.wrapT();
                        }
                    }
                    if (texturing->mTextures.size() > NiTexturingProperty::BumpTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::BumpTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.normalTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.normalWrapU = texture.wrapS();
                            result.normalWrapV = texture.wrapT();
                            if (std::isfinite(texturing->mEnvMapLumaBias.x())
                                && std::isfinite(texturing->mEnvMapLumaBias.y()))
                            {
                                result.emissiveLumaBias
                                    = { texturing->mEnvMapLumaBias.x(), texturing->mEnvMapLumaBias.y() };
                            }
                        }
                    }
                    if (texturing->mTextures.size() > NiTexturingProperty::GlowTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::GlowTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            result.emissiveTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.emissiveWrapU = texture.wrapS();
                            result.emissiveWrapV = texture.wrapT();
                        }
                    }
                    if (texturing->mTextures.size() > NiTexturingProperty::GlossTexture)
                    {
                        const NiTexturingProperty::Texture& texture
                            = texturing->mTextures[NiTexturingProperty::GlossTexture];
                        if (texture.mEnabled && !texture.mSourceTexture.empty())
                        {
                            // Preserve the authored gloss layer through the
                            // neutral specular slot. The Vulkan enchanted
                            // layer multiplies it into the reflected caustic;
                            // dedicated bump/luma bias remains a later gate.
                            result.specularTexture = VFS::Path::toNormalized(texture.mSourceTexture->mFile).value();
                            result.specularWrapU = texture.wrapS();
                            result.specularWrapV = texture.wrapT();
                        }
                    }
                }
                else if (const auto* material = dynamic_cast<const NiMaterialProperty*>(property.getPtr()))
                {
                    result.diffuse = { material->mDiffuse.x(), material->mDiffuse.y(), material->mDiffuse.z(),
                        material->mAlpha };
                    result.emissive = { material->mEmissive.x() * material->mEmissiveMult,
                        material->mEmissive.y() * material->mEmissiveMult,
                        material->mEmissive.z() * material->mEmissiveMult, 1.f };
                    result.glossiness = material->mGlossiness;
                }
            }

            if (!geometry.mAlphaProperty.empty())
            {
                const NiAlphaProperty& alpha = *geometry.mAlphaProperty.getPtr();
                result.alphaBlend = alpha.useAlphaBlending();
                result.alphaTest = alpha.useAlphaTesting();
                result.alphaTestThreshold = alpha.mThreshold;
            }

            if (!geometry.mShaderProperty.empty())
            {
                const BSShaderProperty* shader = geometry.mShaderProperty.getPtr();
                if (const auto* lighting = dynamic_cast<const BSLightingShaderProperty*>(shader))
                {
                    setShaderTexture(result, lighting->mTextureSet, lighting->wrapS(), lighting->wrapT());
                    result.doubleSided = lighting->doubleSided();
                    result.diffuse.w = lighting->mAlpha;
                    result.emissive = { lighting->mEmissive.x() * lighting->mEmissiveMult,
                        lighting->mEmissive.y() * lighting->mEmissiveMult,
                        lighting->mEmissive.z() * lighting->mEmissiveMult, 1.f };
                    result.glossiness = lighting->mGlossiness;
                }
                else if (const auto* ppLighting = dynamic_cast<const BSShaderPPLightingProperty*>(shader))
                {
                    setShaderTexture(result, ppLighting->mTextureSet, ppLighting->wrapS(), ppLighting->wrapT());
                    result.emissive = { ppLighting->mEmissiveColor.x(), ppLighting->mEmissiveColor.y(),
                        ppLighting->mEmissiveColor.z(), ppLighting->mEmissiveColor.w() };
                }
                else if (const auto* noLighting = dynamic_cast<const BSShaderNoLightingProperty*>(shader))
                {
                    if (!noLighting->mFilename.empty())
                    {
                        result.albedoTexture = VFS::Path::toNormalized(noLighting->mFilename).value();
                        result.albedoWrapU = noLighting->wrapS();
                        result.albedoWrapV = noLighting->wrapT();
                    }
                }
                else if (const auto* effect = dynamic_cast<const BSEffectShaderProperty*>(shader))
                {
                    if (!effect->mSourceTexture.empty())
                    {
                        result.albedoTexture = VFS::Path::toNormalized(effect->mSourceTexture).value();
                        result.albedoWrapU = effect->wrapS();
                        result.albedoWrapV = effect->wrapT();
                    }
                    if (!effect->mNormalTexture.empty())
                    {
                        result.normalTexture = VFS::Path::toNormalized(effect->mNormalTexture).value();
                        result.normalWrapU = effect->wrapS();
                        result.normalWrapV = effect->wrapT();
                    }
                    result.diffuse = { effect->mBaseColor.x() * effect->mBaseColorScale,
                        effect->mBaseColor.y() * effect->mBaseColorScale,
                        effect->mBaseColor.z() * effect->mBaseColorScale, effect->mBaseColor.w() };
                    result.emissive = { effect->mEmittanceColor.x(), effect->mEmittanceColor.y(),
                        effect->mEmittanceColor.z(), 1.f };
                    result.doubleSided = effect->doubleSided();
                    result.alphaBlend = result.diffuse.w < 1.f || effect->softEffect() || effect->refraction();
                }
            }

            result.normalMap = !result.normalTexture.empty();

            return result;
        }

        std::shared_ptr<const Render::SkinningData> convertSkinning(const NiGeometry& geometry,
            std::size_t vertexCount)
        {
            if (geometry.mSkin.empty() || geometry.mSkin->mData.empty())
                return {};

            const NiSkinData& source = *geometry.mSkin->mData.getPtr();
            if (source.mBones.empty() || geometry.mSkin->mBones.size() != source.mBones.size())
                return {};

            auto result = std::make_shared<Render::SkinningData>();
            result->vertices.resize(vertexCount);
            const bool hasBoneNames = std::all_of(geometry.mSkin->mBones.begin(), geometry.mSkin->mBones.end(),
                [](const auto& bone) { return !bone.empty() && !bone->mName.empty(); });
            if (hasBoneNames)
                result->boneNames.reserve(geometry.mSkin->mBones.size());
            result->inverseBindMatrices.reserve(source.mBones.size());
            for (std::size_t boneIndex = 0; boneIndex < source.mBones.size(); ++boneIndex)
            {
                const auto& bone = source.mBones[boneIndex];
                if (hasBoneNames)
                    result->boneNames.push_back(geometry.mSkin->mBones[boneIndex]->mName);
                result->inverseBindMatrices.push_back(toRenderMatrix(bone.mTransform));
            }

            for (std::size_t boneIndex = 0; boneIndex < source.mBones.size(); ++boneIndex)
            {
                for (const auto [vertexIndex, weight] : source.mBones[boneIndex].mWeights)
                {
                    if (vertexIndex >= result->vertices.size() || !std::isfinite(weight) || weight <= 0.f)
                        continue;

                    Render::SkinVertex& vertex = result->vertices[vertexIndex];
                    std::size_t slot = 0;
                    for (std::size_t influence = 1; influence < vertex.weights.size(); ++influence)
                    {
                        if (vertex.weights[influence] < vertex.weights[slot])
                            slot = influence;
                    }
                    if (weight > vertex.weights[slot])
                    {
                        vertex.boneIndices[slot] = static_cast<std::uint16_t>(boneIndex);
                        vertex.weights[slot] = weight;
                    }
                }
            }

            for (Render::SkinVertex& vertex : result->vertices)
            {
                float total = 0.f;
                for (const float weight : vertex.weights)
                    total += weight;
                if (total <= 0.f)
                    return {};
                for (float& weight : vertex.weights)
                    weight /= total;
            }
            return result;
        }
    }

    Render::MeshData convertMesh(const NiTriShapeData& source)
    {
        if (source.mTriangles.size() % 3 != 0)
            throw std::runtime_error("NIF triangle index data is not a multiple of three");

        const std::vector<Render::MeshVertexSource> vertices = convertVertices(source);
        Render::MeshData result = Render::makeMeshData(vertices);
        result.indices.reserve(source.mTriangles.size());
        for (unsigned short index : source.mTriangles)
            Render::appendMeshIndex(result, index);
        Render::computeMeshTangents(result);
        return result;
    }

    Render::MeshData convertMesh(const NiTriStripsData& source)
    {
        const std::vector<Render::MeshVertexSource> vertices = convertVertices(source);
        Render::MeshData result = Render::makeMeshData(vertices);
        for (const std::vector<unsigned short>& strip : source.mStrips)
            Render::appendTriangleStripIndices(result, strip);

        Render::computeMeshTangents(result);
        return result;
    }

    const NiParticleSystemController* findParticleController(const NiParticleSystem* system);

    std::shared_ptr<Render::ParticleSimulationData> convertParticleSimulation(
        const NiParticlesData& source, const NiParticleSystem* system)
    {
        if (!system)
            return nullptr;

        auto result = std::make_shared<Render::ParticleSimulationData>();
        for (const NiPSysModifierPtr& modifierReference : system->mModifiers)
        {
            if (modifierReference.empty())
                continue;
            const NiPSysModifier* modifier = modifierReference.getPtr();
            if (!modifier->mActive)
                continue;

            if (const auto* gravity = dynamic_cast<const NiPSysGravityModifier*>(modifier))
            {
                if (gravity->mForceType != ForceType::Wind)
                    continue;
                const float length = gravity->mGravityAxis.length();
                if (std::isfinite(length) && length > 0.f && std::isfinite(gravity->mStrength))
                {
                    result->acceleration.x += gravity->mGravityAxis.x() / length * gravity->mStrength;
                    result->acceleration.y += gravity->mGravityAxis.y() / length * gravity->mStrength;
                    result->acceleration.z += gravity->mGravityAxis.z() / length * gravity->mStrength;
                }
            }
            else if (const auto* drag = dynamic_cast<const NiPSysDragModifier*>(modifier))
            {
                if (std::isfinite(drag->mPercentage) && drag->mPercentage > 0.f)
                    result->drag += drag->mPercentage;
            }
            else if (const auto* growFade = dynamic_cast<const NiPSysGrowFadeModifier*>(modifier))
            {
                if (std::isfinite(growFade->mGrowTime) && growFade->mGrowTime > 0.f)
                    result->growTime = std::max(result->growTime, growFade->mGrowTime);
                if (std::isfinite(growFade->mFadeTime) && growFade->mFadeTime > 0.f)
                    result->fadeTime = std::max(result->fadeTime, growFade->mFadeTime);
                if (std::isfinite(growFade->mBaseScale) && growFade->mBaseScale >= 0.f)
                    result->baseScale = growFade->mBaseScale;
            }
            else if (const auto* rotation = dynamic_cast<const NiPSysRotationModifier*>(modifier))
            {
                if (std::isfinite(rotation->mRotationSpeed))
                    result->rotationSpeed += rotation->mRotationSpeed;
            }
        }

        if (const NiParticleSystemController* controller = findParticleController(system);
            controller && !controller->emitAtVertex())
        {
            const float lifetime = controller->mLifetime;
            const float lifetimeVariation = controller->mLifetimeVariation;
            const float birthRate = controller->noAutoAdjust()
                ? controller->mBirthRate
                : lifetime + lifetimeVariation > 0.f
                ? controller->mParticles.size() / (lifetime + lifetimeVariation * 0.5f)
                : 0.f;
            const std::size_t maxParticles
                = std::max<std::size_t>(source.mNumParticles, std::max<std::size_t>(controller->mNumParticles,
                    controller->mParticles.size()));
            auto emitter = std::make_shared<Render::ParticleSimulationData::Emitter>();
            emitter->startTime = controller->mEmitStartTime;
            emitter->stopTime = controller->mEmitStopTime;
            emitter->birthRate = birthRate;
            emitter->lifetime = lifetime;
            emitter->lifetimeVariation = lifetimeVariation;
            emitter->speed = controller->mSpeed;
            emitter->speedVariation = controller->mSpeedVariation;
            emitter->initialNormal = { controller->mInitialNormal.x(), controller->mInitialNormal.y(),
                controller->mInitialNormal.z() };
            emitter->dimensions = { std::abs(controller->mEmitterDimensions.x()),
                std::abs(controller->mEmitterDimensions.y()), std::abs(controller->mEmitterDimensions.z()) };
            emitter->maxParticles = maxParticles;
            if (emitter->valid())
                result->emitter = std::move(emitter);
        }

        const auto makePlanarCollider = [](float bounce, bool dieOnCollision, bool spawnOnCollision,
                                               const osg::Vec3f& position, const osg::Vec3f& normal,
                                               const osg::Vec3f& xAxis, const osg::Vec3f& yAxis, float planeDistance,
                                               float extentX, float extentY) {
            auto collider = std::make_shared<Render::ParticleSimulationData::Collider>();
            collider->type = Render::ParticleSimulationData::Collider::Type::Planar;
            collider->bounce = bounce;
            collider->dieOnCollision = dieOnCollision;
            collider->spawnOnCollision = spawnOnCollision;
            collider->position = { position.x(), position.y(), position.z() };
            collider->normal = { normal.x(), normal.y(), normal.z() };
            collider->xAxis = { xAxis.x(), xAxis.y(), xAxis.z() };
            collider->yAxis = { yAxis.x(), yAxis.y(), yAxis.z() };
            collider->planeDistance = planeDistance;
            collider->extentX = extentX;
            collider->extentY = extentY;
            return collider;
        };
        const auto makeSphericalCollider = [](float bounce, bool dieOnCollision, bool spawnOnCollision,
                                                  const osg::Vec3f& center, float radius) {
            auto collider = std::make_shared<Render::ParticleSimulationData::Collider>();
            collider->type = Render::ParticleSimulationData::Collider::Type::Spherical;
            collider->bounce = bounce;
            collider->dieOnCollision = dieOnCollision;
            collider->spawnOnCollision = spawnOnCollision;
            collider->position = { center.x(), center.y(), center.z() };
            collider->radius = radius;
            return collider;
        };

        if (const NiParticleSystemController* controller = findParticleController(system); controller
            && controller->mCollider.empty() == false)
        {
            for (NiParticleModifierPtr modifier = controller->mCollider; !modifier.empty(); modifier = modifier->mNext)
            {
                std::shared_ptr<Render::ParticleSimulationData::Collider> collider;
                if (modifier->mRecordType == RC_NiPlanarCollider)
                {
                    const auto* planar = static_cast<const NiPlanarCollider*>(modifier.getPtr());
                    // The legacy operator intentionally swaps the serialized extents when testing its local axes.
                    collider = makePlanarCollider(planar->mBounceFactor, planar->mDieOnCollision,
                        planar->mSpawnOnCollision, planar->mPosition, planar->mPlaneNormal,
                        planar->mXVector, planar->mYVector, planar->mPlaneDistance, planar->mExtents.y(),
                        planar->mExtents.x());
                }
                else if (modifier->mRecordType == RC_NiSphericalCollider)
                {
                    const auto* spherical = static_cast<const NiSphericalCollider*>(modifier.getPtr());
                    collider = makeSphericalCollider(spherical->mBounceFactor, spherical->mDieOnCollision,
                        spherical->mSpawnOnCollision, spherical->mCenter, spherical->mRadius);
                }
                if (collider && collider->valid())
                {
                    result->collider = std::move(collider);
                    break;
                }
            }
        }

        if (!result->collider)
        {
            for (const NiPSysModifierPtr& modifierReference : system->mModifiers)
            {
                const auto* manager = modifierReference.empty()
                    ? nullptr
                    : dynamic_cast<const NiPSysColliderManager*>(modifierReference.getPtr());
                if (!manager || !manager->mActive)
                    continue;
                for (NiPSysColliderPtr colliderReference = manager->mCollider; !colliderReference.empty();
                     colliderReference = colliderReference->mNextCollider)
                {
                    std::shared_ptr<Render::ParticleSimulationData::Collider> collider;
                    if (colliderReference->mRecordType == RC_NiPSysPlanarCollider)
                    {
                        const auto* planar = static_cast<const NiPSysPlanarCollider*>(colliderReference.getPtr());
                        const osg::Vec3f position = planar->mColliderObject.empty()
                            ? osg::Vec3f{}
                            : planar->mColliderObject->mTransform.mTranslation;
                        const osg::Vec3f normal = osg::Vec3f(planar->mXAxis ^ planar->mYAxis);
                        collider = makePlanarCollider(planar->mBounce, planar->mCollideDie, planar->mCollideSpawn, position,
                            normal, planar->mXAxis, planar->mYAxis, 0.f, planar->mWidth, planar->mHeight);
                    }
                    else if (colliderReference->mRecordType == RC_NiPSysSphericalCollider)
                    {
                        const auto* spherical = static_cast<const NiPSysSphericalCollider*>(colliderReference.getPtr());
                        const osg::Vec3f center = spherical->mColliderObject.empty()
                            ? osg::Vec3f{}
                            : spherical->mColliderObject->mTransform.mTranslation;
                        collider = makeSphericalCollider(spherical->mBounce, spherical->mCollideDie,
                            spherical->mCollideSpawn, center, spherical->mRadius);
                    }
                    if (collider && collider->valid())
                    {
                        result->collider = std::move(collider);
                        break;
                    }
                }
                if (result->collider)
                    break;
            }
        }
        return result;
    }

    const NiParticleSystemController* findParticleController(const NiParticleSystem* system)
    {
        if (!system)
            return nullptr;
        for (NiTimeControllerPtr controller = system->mController; !controller.empty(); controller = controller->mNext)
        {
            if (!controller->isActive())
                continue;
            if (controller->mRecordType == RC_NiParticleSystemController
                || controller->mRecordType == RC_NiBSPArrayController)
                return static_cast<const NiParticleSystemController*>(controller.getPtr());
        }
        return nullptr;
    }

    Render::MeshData convertParticles(const NiParticlesData& source, const NiParticleSystem* system)
    {
        Render::MeshData result;
        const auto* systemData = dynamic_cast<const NiPSysData*>(&source);
        const NiParticleSystemController* controller = findParticleController(system);
        const std::vector<NiParticleInfo>* controllerStates
            = controller && !controller->mParticles.empty() ? &controller->mParticles : nullptr;
        const std::vector<NiParticleInfo>* dataStates = systemData
            && systemData->mParticles.size() == source.mVertices.size()
            ? &systemData->mParticles
            : nullptr;
        const std::vector<NiParticleInfo>* particleStates = controllerStates ? controllerStates : dataStates;
        const std::size_t particleCount = std::min<std::size_t>(source.mActiveCount,
            controllerStates ? controllerStates->size() : source.mVertices.size());
        const bool hasParticleState = particleStates != nullptr || controller != nullptr;
        std::shared_ptr<Render::ParticleMeshData> particleState;
        if (hasParticleState)
        {
            particleState = std::make_shared<Render::ParticleMeshData>();
            particleState->states.reserve(particleCount);
        }
        result.vertices.reserve(particleCount * 4);
        result.indices.reserve(particleCount * 6);

        for (std::size_t particle = 0; particle < particleCount; ++particle)
        {
            if (controllerStates && (*controllerStates)[particle].mCode >= source.mVertices.size())
                continue;
            const std::size_t sourceParticle = controllerStates ? (*controllerStates)[particle].mCode : particle;
            float radius = sourceParticle < source.mRadii.size()
                ? source.mRadii[sourceParticle]
                : (!source.mRadii.empty() ? source.mRadii.front() : 1.f);
            if (sourceParticle < source.mSizes.size())
                radius *= source.mSizes[sourceParticle];
            if (controller && std::isfinite(controller->mInitialSize) && controller->mInitialSize > 0.f)
                radius *= controller->mInitialSize;
            if (!std::isfinite(radius) || radius <= 0.f)
                continue;

            const osg::Vec3f& center = source.mVertices[sourceParticle];
            const std::array<osg::Vec3f, 4> corners = {
                osg::Vec3f{ -radius, -radius, 0.f }, osg::Vec3f{ radius, -radius, 0.f },
                osg::Vec3f{ radius, radius, 0.f }, osg::Vec3f{ -radius, radius, 0.f } };
            osg::Quat rotation;
            if (sourceParticle < source.mRotations.size())
                rotation = source.mRotations[sourceParticle];
            else if (sourceParticle < source.mRotationAngles.size() && sourceParticle < source.mRotationAxes.size())
                rotation = osg::Quat(source.mRotationAngles[sourceParticle], source.mRotationAxes[sourceParticle]);
            const std::array<std::array<float, 2>, 4> texcoords = {
                std::array<float, 2>{ 0.f, 0.f }, std::array<float, 2>{ 1.f, 0.f },
                std::array<float, 2>{ 1.f, 1.f }, std::array<float, 2>{ 0.f, 1.f } };
            const bool hasColor = source.mColors.size() == source.mVertices.size();
            const bool hasControllerColor = controller && std::isfinite(controller->mInitialColor.x())
                && std::isfinite(controller->mInitialColor.y()) && std::isfinite(controller->mInitialColor.z())
                && std::isfinite(controller->mInitialColor.w());
            const bool authoredColor = hasColor || hasControllerColor;
            const std::array<float, 4> color = hasControllerColor
                ? std::array<float, 4>{ controller->mInitialColor.x(), controller->mInitialColor.y(),
                      controller->mInitialColor.z(), controller->mInitialColor.w() }
                : hasColor
                ? std::array<float, 4>{ source.mColors[sourceParticle].r(), source.mColors[sourceParticle].g(),
                      source.mColors[sourceParticle].b(), source.mColors[sourceParticle].a() }
                : std::array<float, 4>{ 1.f, 1.f, 1.f, 1.f };
            std::array<Render::MeshVertexSource, 4> quad = {};
            for (std::size_t corner = 0; corner < quad.size(); ++corner)
            {
                const osg::Vec3f rotated = rotation * corners[corner];
                quad[corner] = Render::MeshVertexSource{ { rotated.x(), rotated.y(), 0.f }, {},
                    texcoords[corner], color, false, true, authoredColor };
            }
            Render::MeshData quadMesh = Render::makeMeshData(quad);
            for (Render::MeshVertex& vertex : quadMesh.vertices)
            {
                vertex.tangent[0] = center.x();
                vertex.tangent[1] = center.y();
                vertex.tangent[2] = center.z();
                vertex.tangent[3] = 1.f;
            }
            const std::uint32_t base = static_cast<std::uint32_t>(result.vertices.size());
            result.vertices.insert(result.vertices.end(), quadMesh.vertices.begin(), quadMesh.vertices.end());
            result.indices.insert(result.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
            if (particleState && particleStates)
            {
                const Nif::NiParticleInfo& state = (*particleStates)[particle];
                Render::ParticleState neutralState;
                neutralState.velocity = { state.mVelocity.x(), state.mVelocity.y(), state.mVelocity.z() };
                neutralState.age = state.mAge;
                neutralState.lifespan = state.mLifespan;
                if (systemData && sourceParticle < systemData->mRotationSpeeds.size())
                    neutralState.rotationSpeed = systemData->mRotationSpeeds[sourceParticle];
                particleState->states.push_back(neutralState);
            }
        }

        const std::shared_ptr<Render::ParticleSimulationData> simulation = convertParticleSimulation(source, system);
        if (particleState && (!particleState->states.empty() || (simulation && simulation->emitter)))
        {
            particleState->simulation = simulation;
            result.particles = std::move(particleState);
        }

        return result;
    }

    namespace
    {
        Render::Mat4 toRenderMatrix(const NiTransform& transform)
        {
            Render::Mat4 result = {};
            for (int row = 0; row < 3; ++row)
            {
                for (int col = 0; col < 3; ++col)
                    result.data[col * 4 + row] = transform.mRotation.mValues[row][col] * transform.mScale;
            }
            result.data[12] = transform.mTranslation.x();
            result.data[13] = transform.mTranslation.y();
            result.data[14] = transform.mTranslation.z();
            result.data[15] = 1.0f;
            return result;
        }

        Render::Mat4 toRenderMatrix(const NiQuatTransform& transform)
        {
            const osg::Matrixf matrix = transform.toMatrix();
            Render::Mat4 result = {};
            for (int row = 0; row < 3; ++row)
                for (int col = 0; col < 3; ++col)
                    result.data[col * 4 + row] = matrix(row, col);
            result.data[12] = transform.mTranslation.x();
            result.data[13] = transform.mTranslation.y();
            result.data[14] = transform.mTranslation.z();
            result.data[15] = 1.f;
            return result;
        }

        void collectMeshInstances(const NiAVObject& object, const Render::Mat4& parentTransform,
            std::vector<Render::MeshInstance>& meshes, bool allowSkinning)
        {
            const Render::Mat4 transform = Render::multiply(parentTransform, toRenderMatrix(object.mTransform));
            if (const auto* geometry = dynamic_cast<const NiGeometry*>(&object))
            {
                if (!geometry->mData.empty())
                {
                    if (const auto* particleData = dynamic_cast<const NiParticlesData*>(&geometry->mData.get()))
                    {
                        const auto* particleSystem = dynamic_cast<const NiParticleSystem*>(&object);
                        Render::MeshData mesh = convertParticles(*particleData, particleSystem);
                        if (!mesh.vertices.empty() && !mesh.indices.empty())
                        {
                            mesh.material = convertMaterial(*geometry);
                            mesh.material.doubleSided = true;
                            mesh.material.particleBillboard = true;
                            meshes.push_back({ std::move(mesh), transform });
                        }
                    }
                    else if (const auto* shapeData = dynamic_cast<const NiTriShapeData*>(&geometry->mData.get()))
                    {
                        Render::MeshData mesh = convertMesh(*shapeData);
                        mesh.material = convertMaterial(*geometry);
                        if (allowSkinning)
                            mesh.skinning = convertSkinning(*geometry, mesh.vertices.size());
                        meshes.push_back({ std::move(mesh), transform });
                    }
                    else if (const auto* stripsData = dynamic_cast<const NiTriStripsData*>(&geometry->mData.get()))
                    {
                        Render::MeshData mesh = convertMesh(*stripsData);
                        mesh.material = convertMaterial(*geometry);
                        if (allowSkinning)
                            mesh.skinning = convertSkinning(*geometry, mesh.vertices.size());
                        meshes.push_back({ std::move(mesh), transform });
                    }
                }
            }

            if (const auto* node = dynamic_cast<const NiNode*>(&object))
            {
                for (const auto& child : node->mChildren)
                {
                    if (!child.empty())
                        collectMeshInstances(*child.getPtr(), transform, meshes, allowSkinning);
                }
            }
        }
    }

    std::vector<Render::MeshInstance> collectMeshInstances(FileView file)
    {
        Render::Mat4 identity = {};
        identity.data[0] = 1.0f;
        identity.data[5] = 1.0f;
        identity.data[10] = 1.0f;
        identity.data[15] = 1.0f;

        std::vector<Render::MeshInstance> meshes;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
        {
            if (const auto* root = dynamic_cast<const NiAVObject*>(file.getRoot(i)))
                collectMeshInstances(*root, identity, meshes, file.getUseSkinning());
        }
        return meshes;
    }

    std::vector<Render::Mat4> collectBonePose(
        FileView file, std::span<const std::string> boneNames, float time, std::string_view group,
        std::string_view startKey, std::string_view stopKey)
    {
        const std::array<FileView, 1> files{ file };
        return collectBonePose(files, boneNames, time, group, startKey, stopKey);
    }

    std::vector<Render::Mat4> collectBonePose(
        std::span<const FileView> files, std::span<const std::string> boneNames, float time,
        std::string_view group, std::string_view startKey, std::string_view stopKey)
    {
        if (boneNames.empty() || !std::isfinite(time))
            return {};

        const Render::Mat4 identity = Render::identityMat4();
        std::unordered_map<std::string, Render::Mat4> transforms;
        std::unordered_map<std::string, int> sequencePriorities;
        std::unordered_map<std::string, SequenceTransformState> weightedSequences;
        for (const FileView& file : files)
        {
            float sampleTime = time;
            std::optional<float> segmentStart;
            if (!group.empty() && !startKey.empty())
            {
                segmentStart = findTextKeyTime(file, std::string(group) + ": " + std::string(startKey));
                if (segmentStart)
                    sampleTime += *segmentStart;
            }
            if (segmentStart && !stopKey.empty())
            {
                if (const std::optional<float> stop
                    = findTextKeyTime(file, std::string(group) + ": " + std::string(stopKey));
                    stop && *stop >= *segmentStart)
                    sampleTime = std::min(sampleTime, *stop);
            }

            for (std::size_t i = 0; i < file.numRoots(); ++i)
            {
                if (const auto* root = dynamic_cast<const NiAVObject*>(file.getRoot(i)))
                    collectBoneTransforms(*root, identity, sampleTime, transforms);
                else if (const auto* stream = dynamic_cast<const NiSequenceStreamHelper*>(file.getRoot(i)))
                    collectSequenceTransforms(
                        *stream, time, group, startKey, stopKey, transforms, sequencePriorities, weightedSequences);
                else if (const auto* controllerSequence = dynamic_cast<const NiSequence*>(file.getRoot(i)))
                    collectControllerSequenceTransforms(
                        *controllerSequence, time, group, startKey, stopKey, transforms, sequencePriorities,
                        weightedSequences);
            }
        }

        std::vector<Render::Mat4> result;
        result.reserve(boneNames.size());
        for (const std::string& name : boneNames)
        {
            const auto found = transforms.find(normalizedBoneName(name));
            if (found == transforms.end())
                return {};
            result.push_back(found->second);
        }
        return result;
    }

    std::optional<float> findTextKeyTime(FileView file, std::string_view textKey)
    {
        for (std::size_t i = 0; i < file.numRoots(); ++i)
            if (const Record* root = file.getRoot(i))
                if (const auto time = findTextKeyTime(*root, textKey))
                    return time;
        return std::nullopt;
    }

    std::vector<Render::AnimationTextKey> collectTextKeys(FileView file, std::string_view group)
    {
        std::vector<Render::AnimationTextKey> result;
        if (group.empty())
            return result;
        for (std::size_t i = 0; i < file.numRoots(); ++i)
            if (const Record* root = file.getRoot(i))
                collectTextKeys(*root, group, result);
        std::stable_sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.time < rhs.time;
        });
        return result;
    }
}
