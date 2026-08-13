#ifndef OPENMW_COMPONENTS_RENDER_WORLD_H
#define OPENMW_COMPONENTS_RENDER_WORLD_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "math.hpp"
#include "terrain.hpp"

namespace Render
{
    struct WeatherEffects
    {
        bool enabled = false;
        float alpha = 0.f;
        float diameter = 0.f;
        float minHeight = 0.f;
        float maxHeight = 0.f;
        float speed = 0.f;
        int maxParticles = 0;
        bool snow = false;
        // Horizontal storm direction plus downward fall. This keeps
        // precipitation motion renderer-neutral instead of baking a vertical
        // particle assumption into the Vulkan consumer.
        Vec3 fallDirection{ 0.f, 0.f, -1.f };

        bool valid() const
        {
            if (!enabled)
                return true;
            return std::isfinite(alpha) && alpha >= 0.f && alpha <= 1.f && std::isfinite(diameter)
                && diameter > 0.f && std::isfinite(minHeight) && std::isfinite(maxHeight)
                && maxHeight > minHeight && std::isfinite(speed) && speed >= 0.f && maxParticles >= 0
                && Render::valid(fallDirection) && (std::abs(fallDirection.x) + std::abs(fallDirection.y)
                    + std::abs(fallDirection.z) > 0.0001f);
        }
    };

    struct WaterSurface
    {
        float minX = 0.f;
        float maxX = 0.f;
        float minY = 0.f;
        float maxY = 0.f;
        float level = 0.f;

        bool valid() const
        {
            return std::isfinite(minX) && std::isfinite(maxX) && std::isfinite(minY) && std::isfinite(maxY)
                && std::isfinite(level) && minX < maxX && minY < maxY;
        }
    };

    struct WaterRipple
    {
        Vec3 position{ 0.f, 0.f, 0.f };
        float age = 0.f;
        float size = 12.f;

        bool valid() const
        {
            return Render::valid(position) && std::isfinite(age) && age >= 0.f && std::isfinite(size) && size > 0.f;
        }
    };

    struct ObjectTransform
    {
        Vec3 position{};
        Quat rotation{ 0.f, 0.f, 0.f, 1.f };
        Vec3 scale{ 1.f, 1.f, 1.f };

        bool valid() const
        {
            return Render::valid(position) && Render::valid(rotation) && Render::valid(scale);
        }
    };

    // Renderer-neutral state for one loaded world reference. The id is owned by
    // the scene bridge and is intentionally opaque to backends.
    struct WorldObject
    {
        enum class PointLightAnimation
        {
            None,
            Flicker,
            FlickerSlow,
            Pulse,
            PulseSlow
        };

        struct AnimationLayer
        {
            std::string id;
            std::string group;
            std::string startKey;
            std::string stopKey;
            float time = 0.f;
            bool looping = false;
            unsigned mask = 0xfu;
            int priority = 0;
        };

        struct Attachment
        {
            std::string id;
            std::string model;
            std::string bone;
            bool visible = true;
            Vec4 emissiveColor{};
            bool emissiveOverride = false;
        };

        uint64_t id = 0;
        std::string model;
        // Additional models are composed at the same effect transform. This
        // preserves multi-part VFX without exposing legacy scene-graph nodes.
        std::vector<std::string> additionalModels;
        // Actor animation source order is selected by the world owner. Empty
        // means the resource manager may use model-local discovery defaults.
        std::vector<std::string> animationSources;
        ObjectTransform transform;
        bool visible = true;
        bool dynamic = false;
        bool staticInstance = false;
        // Optional frame pose supplied by the animation owner. The matrices
        // use the skinning order of the resolved mesh and contain no backend
        // or scene-graph types.
        std::vector<Mat4> boneMatrices;
        // Optional texture replacement used by explicitly identified world VFX.
        std::string textureOverride;
        // World effects retain whether gameplay requested looping playback.
        // Controller timing is advanced by the world owner; mesh controller
        // playback remains a separate animation milestone.
        bool looping = false;
        // A zero duration means the resource did not expose a controller
        // interval. Such effects remain explicitly removable by gameplay.
        float animationDuration = 0.f;
        float animationTime = 0.f;
        // Neutral gameplay animation selection. Renderer code consumes this
        // name and segment but does not own gameplay priority or blend policy.
        std::string animationGroup;
        std::string animationStartKey;
        std::string animationStopKey;
        bool animationLooping = false;
        // Optional overlays are resolved against the same skin in priority
        // order. The base animation above remains the compatibility path for
        // callers that only need one full-body group.
        std::vector<AnimationLayer> animationLayers;
        // Magic VFX use the legacy first-root texture replacement rule when
        // their flattened neutral mesh list is submitted.
        bool magicVfx = false;
        // Legacy VFX may override the scene ambient term with white light.
        bool ambientOverride = false;
        // Optional gameplay-provided point light, without exposing backend
        // light or scene-graph types to the neutral scene.
        Vec4 pointLightColor{};
        float pointLightRadius = 0.f;
        bool pointLightNegative = false;
        PointLightAnimation pointLightAnimation = PointLightAnimation::None;
        float pointLightBrightness = 0.675f;
        float pointLightPhase = 0.5f;
        float pointLightTicks = 0.f;
        Vec4 emissiveColor{};
        bool emissiveOverride = false;
        float opacity = 1.f;
        bool active = true;
        std::vector<Attachment> attachments;
    };

    // A cell snapshot is updated by the world lifecycle, not by a renderer.
    struct CellScene
    {
        const void* key = nullptr;
        bool exterior = false;
        int gridX = 0;
        int gridY = 0;
        std::string worldspace;
        std::string name;
        std::vector<WorldObject> objects;
        std::vector<TerrainTile> terrainTiles;
        std::optional<WaterSurface> water;

        WorldObject* findObject(uint64_t id)
        {
            const auto found = std::find_if(objects.begin(), objects.end(), [id](const WorldObject& object) {
                return object.id == id;
            });
            return found == objects.end() ? nullptr : &*found;
        }

        const WorldObject* findObject(uint64_t id) const
        {
            const auto found = std::find_if(objects.begin(), objects.end(), [id](const WorldObject& object) {
                return object.id == id;
            });
            return found == objects.end() ? nullptr : &*found;
        }

        bool eraseObject(uint64_t id)
        {
            const auto oldSize = objects.size();
            std::erase_if(objects, [id](const WorldObject& object) { return object.id == id; });
            return objects.size() != oldSize;
        }
    };

    // Renderer-neutral ownership for loaded cells and their object identity.
    // The keys are opaque engine-owned handles; no renderer or game type leaks
    // into this component.
    class WorldScene
    {
        struct ObjectLocation
        {
            const void* cell;
            uint64_t id;
        };

        std::unordered_map<const void*, CellScene> mCells;
        std::unordered_map<const void*, ObjectLocation> mObjects;
        std::map<std::string, WorldObject, std::less<>> mEffects;
        std::vector<TerrainRegion> mTerrainRegions;
        std::string mActiveWorldspace;
        SceneData mSceneData{};
        WeatherEffects mWeatherEffects;
        float mWeatherTime = 0.f;
        std::vector<WaterRipple> mWaterRipples;
        bool mWaterEnabled = true;
        uint64_t mNextObjectId = 1;

        CellScene& ensureCell(const void* cell, bool exterior, int gridX, int gridY, std::string_view name,
            std::string_view worldspace)
        {
            auto iter = mCells.try_emplace(cell).first;

            CellScene& scene = iter->second;
            scene.key = cell;
            scene.exterior = exterior;
            scene.gridX = gridX;
            scene.gridY = gridY;
            scene.worldspace = worldspace;
            scene.name = name;
            return scene;
        }

        void rekeyObject(const void* oldKey, const void* newKey, const ObjectLocation& location)
        {
            if (oldKey == newKey)
                return;
            mObjects.erase(oldKey);
            mObjects.emplace(newKey, location);
        }

        static void resetPointLightAnimation(WorldObject& object)
        {
            object.pointLightBrightness = 0.675f;
            object.pointLightTicks = 0.f;
            object.pointLightPhase = 0.25f + static_cast<float>(object.id % 1000) / 1000.f * 0.75f;
        }

        static void updatePointLightAnimation(WorldObject& object, float duration)
        {
            if (object.pointLightAnimation == WorldObject::PointLightAnimation::None)
                return;

            object.pointLightTicks = duration * 15.f * 0.25f + object.pointLightTicks * 0.75f;
            const bool fast = object.pointLightAnimation == WorldObject::PointLightAnimation::Flicker
                || object.pointLightAnimation == WorldObject::PointLightAnimation::Pulse;
            const float speed = fast ? 0.1f : 0.05f;
            if (object.pointLightBrightness >= object.pointLightPhase)
                object.pointLightBrightness -= object.pointLightTicks * speed;
            else
                object.pointLightBrightness += object.pointLightTicks * speed;

            if (std::abs(object.pointLightBrightness - object.pointLightPhase) < speed)
            {
                const bool flicker = object.pointLightAnimation == WorldObject::PointLightAnimation::Flicker
                    || object.pointLightAnimation == WorldObject::PointLightAnimation::FlickerSlow;
                if (flicker)
                    object.pointLightPhase = 0.25f + static_cast<float>((object.id * 37) % 1000) / 1000.f * 0.75f;
                else
                    object.pointLightPhase = object.pointLightPhase <= 0.5f ? 1.f : 0.25f;
            }
            object.pointLightBrightness = std::clamp(object.pointLightBrightness, 0.f, 1.f);
        }

        void refreshPointLights()
        {
            mSceneData.pointLightPositions = {};
            mSceneData.pointLightColorsAndRadii = {};
            mSceneData.pointLightCount = {};

            std::size_t count = 0;
            for (const auto& [id, effect] : mEffects)
            {
                if (count >= SceneData::maxPointLights || !effect.active || !effect.visible
                    || !valid(effect.transform.position) || !valid(effect.pointLightColor)
                    || !valid(effect.pointLightRadius) || effect.pointLightRadius <= 0.f)
                    continue;

                mSceneData.pointLightPositions[count]
                    = { effect.transform.position.x, effect.transform.position.y, effect.transform.position.z, 1.f };
                mSceneData.pointLightColorsAndRadii[count]
                        = { effect.pointLightColor.x, effect.pointLightColor.y, effect.pointLightColor.z,
                            effect.pointLightRadius };
                ++count;
            }
            for (const auto& [cellKey, cell] : mCells)
            {
                for (const WorldObject& object : cell.objects)
                {
                    if (count >= SceneData::maxPointLights || !object.active || !object.visible
                        || !valid(object.transform.position) || !valid(object.pointLightColor)
                        || !valid(object.pointLightRadius) || object.pointLightRadius <= 0.f)
                        continue;

                    mSceneData.pointLightPositions[count]
                        = { object.transform.position.x, object.transform.position.y, object.transform.position.z,
                            object.pointLightNegative ? -1.f : 1.f };
                    const float brightness = object.pointLightAnimation == WorldObject::PointLightAnimation::None
                        ? 1.f
                        : object.pointLightBrightness;
                    mSceneData.pointLightColorsAndRadii[count]
                        = { object.pointLightColor.x * brightness, object.pointLightColor.y * brightness,
                            object.pointLightColor.z * brightness,
                            object.pointLightRadius };
                    ++count;
                }
            }
            mSceneData.pointLightCount.x = static_cast<float>(count);
        }

        template <class Update>
        bool updateObjectTransform(const void* objectKey, Update&& update)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr)
                return false;
            update(object->transform);
            refreshPointLights();
            return true;
        }

    public:
        void setActiveWorldspace(std::string_view worldspace) { mActiveWorldspace = worldspace; }

        std::string_view activeWorldspace() const { return mActiveWorldspace; }

        SceneData& sceneData() { return mSceneData; }

        void setWeatherEffects(const WeatherEffects& effects)
        {
            mWeatherEffects = effects.valid() ? effects : WeatherEffects{};
            if (!mWeatherEffects.enabled)
                mWeatherTime = 0.f;
        }

        void clearWeatherEffects() { setWeatherEffects({}); }

        const WeatherEffects& weatherEffects() const { return mWeatherEffects; }

        float weatherTime() const { return mWeatherTime; }

        void recordCell(const void* cellKey, bool exterior, int gridX, int gridY, std::string_view name,
            std::string_view worldspace = {}, std::optional<WaterSurface> water = {})
        {
            if (cellKey == nullptr)
                return;
            CellScene& scene = ensureCell(cellKey, exterior, gridX, gridY, name, worldspace);
            std::erase_if(scene.objects, [](const WorldObject& object) { return object.staticInstance; });
            scene.water = std::move(water);
        }

        void setWaterEnabled(bool enabled) { mWaterEnabled = enabled; }

        bool waterEnabled() const { return mWaterEnabled; }

        void emitWaterRipple(const Vec3& position, float size = 12.f)
        {
            WaterRipple ripple;
            ripple.position = position;
            ripple.size = size;
            if (ripple.valid())
            {
                if (mWaterRipples.size() >= 128)
                    mWaterRipples.erase(mWaterRipples.begin());
                mWaterRipples.push_back(ripple);
            }
        }

        const std::vector<WaterRipple>& waterRipples() const { return mWaterRipples; }

        bool updateWaterLevel(const void* cellKey, float level)
        {
            const auto found = mCells.find(cellKey);
            if (found == mCells.end() || !found->second.water)
                return false;
            found->second.water->level = level;
            return true;
        }

        bool recordEffect(std::string_view effectId, std::string_view model, const Vec3& position, float scale,
            std::string_view textureOverride = {}, bool looping = false, float animationDuration = 0.f,
            bool magicVfx = false, bool ambientOverride = false, const Vec4& pointLightColor = {},
            float pointLightRadius = 0.f, const Vec4& emissiveColor = {}, bool emissiveOverride = false,
            std::span<const std::string> additionalModels = {})
        {
            if (effectId.empty() || model.empty() || !valid(position) || !valid(scale) || scale <= 0.f
                || !valid(pointLightColor) || !valid(pointLightRadius) || pointLightRadius < 0.f
                || (pointLightRadius > 0.f
                    && (pointLightColor.x < 0.f || pointLightColor.y < 0.f || pointLightColor.z < 0.f))
                || !valid(emissiveColor) || (emissiveOverride && (emissiveColor.x < 0.f || emissiveColor.y < 0.f
                    || emissiveColor.z < 0.f || emissiveColor.w <= 0.f)))
                return false;
            WorldObject effect;
            effect.id = mNextObjectId++;
            if (effect.id == 0)
                effect.id = mNextObjectId++;
            effect.model = model;
            effect.additionalModels.assign(additionalModels.begin(), additionalModels.end());
            effect.transform.position = position;
            effect.transform.scale = { scale, scale, scale };
            effect.textureOverride = textureOverride;
            effect.looping = looping;
            effect.animationLooping = looping;
            effect.animationDuration = valid(animationDuration) && animationDuration > 0.f ? animationDuration : 0.f;
            effect.magicVfx = magicVfx;
            effect.ambientOverride = ambientOverride;
            effect.pointLightColor = pointLightColor;
            effect.pointLightRadius = pointLightRadius;
            effect.emissiveColor = emissiveColor;
            effect.emissiveOverride = emissiveOverride;
            mEffects[std::string(effectId)] = std::move(effect);
            refreshPointLights();
            return true;
        }

        bool removeEffect(std::string_view effectId)
        {
            const bool removed = mEffects.erase(std::string(effectId)) != 0;
            if (removed)
                refreshPointLights();
            return removed;
        }

        bool updateEffect(std::string_view effectId, const Vec3& position, const Quat& rotation)
        {
            const auto found = mEffects.find(std::string(effectId));
            if (found == mEffects.end() || !valid(position) || !valid(rotation))
                return false;
            found->second.transform.position = position;
            found->second.transform.rotation = rotation;
            refreshPointLights();
            return true;
        }

        void updateEffects(float duration)
        {
            if (!valid(duration) || duration <= 0.f)
                return;
            mSceneData.effectTime.x = std::fmod(mSceneData.effectTime.x + duration, 4096.f);
            if (mWeatherEffects.enabled && mWeatherEffects.speed > 0.f)
                mWeatherTime = std::fmod(mWeatherTime + duration, 3600.f);

            for (WaterRipple& ripple : mWaterRipples)
                ripple.age += duration;
            std::erase_if(mWaterRipples, [](const WaterRipple& ripple) { return ripple.age >= 1.5f; });

            for (auto& [cellKey, cell] : mCells)
                for (WorldObject& object : cell.objects)
                {
                    updatePointLightAnimation(object, duration);
                    if (object.dynamic)
                    {
                        object.animationTime += duration;
                        for (WorldObject::AnimationLayer& layer : object.animationLayers)
                            layer.time += duration;
                    }
                }

            for (auto iter = mEffects.begin(); iter != mEffects.end();)
            {
                WorldObject& effect = iter->second;
                if (effect.animationDuration <= 0.f)
                {
                    ++iter;
                    continue;
                }

                effect.animationTime += duration;
                if (effect.animationTime < effect.animationDuration)
                {
                    ++iter;
                    continue;
                }

                if (!effect.looping)
                {
                    iter = mEffects.erase(iter);
                    continue;
                }

                effect.animationTime = std::fmod(effect.animationTime, effect.animationDuration);
                ++iter;
            }
            refreshPointLights();
        }

        void clearEffects()
        {
            mEffects.clear();
            refreshPointLights();
        }

        std::vector<const WorldObject*> effectsInOrder() const
        {
            std::vector<const WorldObject*> result;
            result.reserve(mEffects.size());
            for (const auto& [id, effect] : mEffects)
                result.push_back(&effect);
            return result;
        }

        void setTerrainTiles(const void* cellKey, std::vector<TerrainTile> tiles)
        {
            if (cellKey == nullptr)
                return;
            const auto found = mCells.find(cellKey);
            if (found != mCells.end())
                found->second.terrainTiles = std::move(tiles);
        }

        void setTerrainRegions(std::vector<TerrainRegion> regions) { mTerrainRegions = std::move(regions); }

        const std::vector<TerrainRegion>& terrainRegions() const { return mTerrainRegions; }

        // Add renderer-owned static instances whose lifetime is tied to a cell
        // but which do not have an engine reference identity, such as groundcover.
        void recordStaticObject(const void* cellKey, bool exterior, int gridX, int gridY, std::string_view cellName,
            std::string_view model, const ObjectTransform& transform, bool visible,
            std::string_view worldspace = {})
        {
            if (cellKey == nullptr || model.empty())
                return;

            WorldObject object;
            object.id = mNextObjectId++;
            if (object.id == 0)
                object.id = mNextObjectId++;
            object.model = model;
            object.transform = transform;
            object.visible = visible;
            object.dynamic = false;
            object.staticInstance = true;
            ensureCell(cellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(object));
        }

        void recordObject(const void* objectKey, const void* cellKey, bool exterior, int gridX, int gridY,
            std::string_view cellName, std::string_view model, const ObjectTransform& transform, bool visible,
            std::string_view worldspace = {}, bool dynamic = false,
            std::span<const std::string> animationSources = {}, const Vec4& pointLightColor = {},
            float pointLightRadius = 0.f,
            WorldObject::PointLightAnimation pointLightAnimation = WorldObject::PointLightAnimation::None,
            bool pointLightNegative = false)
        {
            if (objectKey == nullptr || cellKey == nullptr || model.empty() || !valid(pointLightColor)
                || !valid(pointLightRadius) || pointLightRadius < 0.f
                || (pointLightRadius > 0.f
                    && (pointLightColor.x < 0.f || pointLightColor.y < 0.f || pointLightColor.z < 0.f)))
            {
                removeObject(objectKey);
                return;
            }

            const auto found = mObjects.find(objectKey);
            if (found != mObjects.end())
            {
                const ObjectLocation location = found->second;
                if (location.cell != cellKey)
                {
                    if (updateObjectCell(objectKey, objectKey, cellKey, exterior, gridX, gridY, cellName, worldspace))
                        return recordObject(
                            objectKey, cellKey, exterior, gridX, gridY, cellName, model, transform, visible, worldspace,
                            dynamic, animationSources, pointLightColor, pointLightRadius, pointLightAnimation,
                            pointLightNegative);
                    mObjects.erase(found);
                }
                else if (CellScene* scene = findCell(location.cell))
                {
                    if (WorldObject* object = scene->findObject(location.id))
                    {
                        const bool modelChanged = object->model != model;
                        object->model = model;
                        object->animationSources.assign(animationSources.begin(), animationSources.end());
                        object->transform = transform;
                        object->visible = visible;
                        object->dynamic = dynamic;
                        object->pointLightColor = pointLightColor;
                        object->pointLightRadius = pointLightRadius;
                        object->pointLightNegative = pointLightNegative;
                        if (object->pointLightAnimation != pointLightAnimation)
                        {
                            object->pointLightAnimation = pointLightAnimation;
                            resetPointLightAnimation(*object);
                        }
                        if (!dynamic || modelChanged)
                        {
                            object->boneMatrices.clear();
                            object->animationTime = 0.f;
                            object->animationGroup.clear();
                            object->animationLooping = false;
                            object->animationLayers.clear();
                            object->attachments.clear();
                        }
                        refreshPointLights();
                        return;
                    }
                }
                mObjects.erase(found);
            }

            WorldObject object;
            object.id = mNextObjectId++;
            if (object.id == 0)
                object.id = mNextObjectId++;
            const uint64_t id = object.id;
            object.model = model;
            object.animationSources.assign(animationSources.begin(), animationSources.end());
            object.transform = transform;
            object.visible = visible;
            object.dynamic = dynamic;
            object.pointLightColor = pointLightColor;
            object.pointLightRadius = pointLightRadius;
            object.pointLightNegative = pointLightNegative;
            object.pointLightAnimation = pointLightAnimation;
            resetPointLightAnimation(object);
            ensureCell(cellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(object));
            mObjects.emplace(objectKey, ObjectLocation{ cellKey, id });
            refreshPointLights();
        }

        bool updateObjectPosition(const void* objectKey, const Vec3& position)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.position = position; });
        }

        bool updateObjectRotation(const void* objectKey, const Quat& rotation)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.rotation = rotation; });
        }

        bool updateObjectScale(const void* objectKey, const Vec3& scale)
        {
            return updateObjectTransform(objectKey, [&](ObjectTransform& transform) { transform.scale = scale; });
        }

        bool updateObjectVisibility(const void* objectKey, float value)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end() || !std::isfinite(value))
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr)
                return false;
            object->opacity = std::clamp(value, 0.f, 1.f);
            object->visible = object->opacity > 0.f;
            refreshPointLights();
            return true;
        }

        bool updateObjectActive(const void* objectKey, bool value)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr)
                return false;
            object->active = value;
            refreshPointLights();
            return true;
        }

        bool updateObjectPose(const void* objectKey, std::vector<Mat4> boneMatrices)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;
            object->boneMatrices = std::move(boneMatrices);
            return true;
        }

        bool updateObjectAnimation(const void* objectKey, std::string_view group,
            std::optional<float> animationTime = std::nullopt, std::string_view startKey = {},
            std::string_view stopKey = {}, bool looping = false)
        {
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;
            if (object->animationGroup != group || object->animationStartKey != startKey
                || object->animationStopKey != stopKey)
            {
                object->animationGroup = group;
                object->animationStartKey = startKey;
                object->animationStopKey = stopKey;
                object->animationLooping = looping;
                object->animationTime = 0.f;
                object->boneMatrices.clear();
            }
            else
                object->animationLooping = looping;
            if (animationTime && std::isfinite(*animationTime) && *animationTime >= 0.f)
                object->animationTime = *animationTime;
            return true;
        }

        bool updateObjectAnimationLayer(const void* objectKey, std::string_view layerId, std::string_view group,
            std::optional<float> animationTime = std::nullopt, std::string_view startKey = {},
            std::string_view stopKey = {}, bool looping = false, unsigned mask = 0xfu, int priority = 0)
        {
            if (layerId.empty() || group.empty() || mask == 0)
                return false;
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;

            auto layer = std::find_if(object->animationLayers.begin(), object->animationLayers.end(),
                [&](const WorldObject::AnimationLayer& candidate) { return candidate.id == layerId; });
            if (layer == object->animationLayers.end())
            {
                layer = object->animationLayers.emplace(object->animationLayers.end(), WorldObject::AnimationLayer{});
                layer->id = layerId;
                layer->time = 0.f;
            }
            if (layer->group != group || layer->startKey != startKey || layer->stopKey != stopKey)
            {
                layer->time = 0.f;
                object->boneMatrices.clear();
            }
            layer->group = group;
            layer->startKey = startKey;
            layer->stopKey = stopKey;
            layer->looping = looping;
            layer->mask = mask;
            layer->priority = priority;
            if (animationTime && std::isfinite(*animationTime) && *animationTime >= 0.f)
                layer->time = *animationTime;
            return true;
        }

        bool removeObjectAnimationLayer(const void* objectKey, std::string_view layerId)
        {
            if (layerId.empty())
                return false;
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;
            const auto layer = std::find_if(object->animationLayers.begin(), object->animationLayers.end(),
                [&](const WorldObject::AnimationLayer& candidate) { return candidate.id == layerId; });
            if (layer == object->animationLayers.end())
                return false;
            object->animationLayers.erase(layer);
            object->boneMatrices.clear();
            return true;
        }

        bool updateObjectAttachment(const void* objectKey, std::string_view attachmentId, std::string_view model,
            std::string_view bone, bool visible, const Vec4& emissiveColor = {}, bool emissiveOverride = false)
        {
            if (objectKey == nullptr || attachmentId.empty() || !valid(emissiveColor)
                || (emissiveOverride && (emissiveColor.x < 0.f || emissiveColor.y < 0.f
                    || emissiveColor.z < 0.f || emissiveColor.w <= 0.f)))
                return false;
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            WorldObject* const object = scene->second.findObject(found->second.id);
            if (object == nullptr || !object->dynamic)
                return false;

            const auto attachment = std::find_if(object->attachments.begin(), object->attachments.end(),
                [&](const WorldObject::Attachment& candidate) { return candidate.id == attachmentId; });
            if (model.empty())
            {
                if (attachment != object->attachments.end())
                    object->attachments.erase(attachment);
                return true;
            }

            if (attachment == object->attachments.end())
                object->attachments.push_back({ std::string(attachmentId), std::string(model), std::string(bone), visible,
                    emissiveColor, emissiveOverride });
            else
            {
                attachment->model = model;
                attachment->bone = bone;
                attachment->visible = visible;
                attachment->emissiveColor = emissiveColor;
                attachment->emissiveOverride = emissiveOverride;
            }
            return true;
        }

        bool isObjectAnimationPlaying(const void* objectKey, std::string_view group, float duration) const
        {
            if (objectKey == nullptr || group.empty() || !std::isfinite(duration) || duration <= 0.f)
                return false;
            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return false;
            const auto scene = mCells.find(found->second.cell);
            if (scene == mCells.end())
                return false;
            const WorldObject* const object = scene->second.findObject(found->second.id);
            return object != nullptr && object->dynamic && object->animationGroup == group
                && (object->animationLooping || object->animationTime < duration);
        }

        // Sorting the owned cells makes backend input deterministic without
        // maintaining a second cell-order index.
        std::vector<const CellScene*> cellsInOrder(std::string_view worldspace = {}) const
        {
            std::vector<const CellScene*> result;
            result.reserve(mCells.size());
            for (const auto& [cellKey, cell] : mCells)
            {
                if (worldspace.empty() || cell.worldspace == worldspace)
                    result.push_back(&cell);
            }
            std::stable_sort(result.begin(), result.end(), [](const CellScene* lhs, const CellScene* rhs) {
                if (lhs->exterior != rhs->exterior)
                    return lhs->exterior > rhs->exterior;
                if (lhs->worldspace != rhs->worldspace)
                    return lhs->worldspace < rhs->worldspace;
                if (lhs->gridX != rhs->gridX)
                    return lhs->gridX < rhs->gridX;
                if (lhs->gridY != rhs->gridY)
                    return lhs->gridY < rhs->gridY;
                return lhs->name < rhs->name;
            });
            return result;
        }

        CellScene* findCell(const void* cellKey)
        {
            const auto found = mCells.find(cellKey);
            return found == mCells.end() ? nullptr : &found->second;
        }

        void removeObject(const void* objectKey)
        {
            if (objectKey == nullptr)
                return;

            const auto found = mObjects.find(objectKey);
            if (found == mObjects.end())
                return;
            const auto scene = mCells.find(found->second.cell);
            if (scene != mCells.end())
                scene->second.eraseObject(found->second.id);
            mObjects.erase(found);
        }

        bool updateObjectCell(const void* oldKey, const void* newKey, const void* newCellKey, bool exterior, int gridX,
            int gridY, std::string_view cellName = {}, std::string_view worldspace = {})
        {
            if (oldKey == nullptr || newKey == nullptr || newCellKey == nullptr)
                return false;

            const auto found = mObjects.find(oldKey);
            if (found == mObjects.end())
                return false;

            const ObjectLocation location = found->second;
            if (location.cell == newCellKey)
            {
                rekeyObject(oldKey, newKey, location);
                return true;
            }

            auto oldScene = mCells.find(location.cell);
            if (oldScene == mCells.end())
                return false;
            WorldObject* object = oldScene->second.findObject(location.id);
            if (object == nullptr)
                return false;

            WorldObject moved = std::move(*object);
            oldScene->second.eraseObject(location.id);
            ensureCell(newCellKey, exterior, gridX, gridY, cellName, worldspace).objects.push_back(std::move(moved));

            mObjects.erase(found);
            mObjects.emplace(newKey, ObjectLocation{ newCellKey, location.id });
            return true;
        }

        void removeCell(const void* cellKey)
        {
            if (cellKey == nullptr)
                return;
            std::erase_if(mObjects, [cellKey](const auto& entry) { return entry.second.cell == cellKey; });
            mCells.erase(cellKey);
        }

        // Reset renderer-neutral world ownership when the game unloads its
        // world. This is separate from removeCell so a backend can retain
        // stable cell ordering during ordinary streaming.
        void clear()
        {
            mCells.clear();
            mObjects.clear();
            mEffects.clear();
            mTerrainRegions.clear();
            mActiveWorldspace.clear();
            mSceneData = {};
            mWeatherEffects = {};
            mWeatherTime = 0.f;
            mWaterRipples.clear();
            mWaterEnabled = true;
            mNextObjectId = 1;
        }
    };

    inline Mat4 makeObjectTransformMatrix(const ObjectTransform& transform)
    {
        const float x = transform.rotation.x;
        const float y = transform.rotation.y;
        const float z = transform.rotation.z;
        const float w = transform.rotation.w;

        const float xx = x * x;
        const float yy = y * y;
        const float zz = z * z;
        const float xy = x * y;
        const float xz = x * z;
        const float yz = y * z;
        const float wx = w * x;
        const float wy = w * y;
        const float wz = w * z;

        Mat4 result = {};
        result.data[0] = (1.f - 2.f * (yy + zz)) * transform.scale.x;
        result.data[1] = (2.f * (xy + wz)) * transform.scale.x;
        result.data[2] = (2.f * (xz - wy)) * transform.scale.x;
        result.data[4] = (2.f * (xy - wz)) * transform.scale.y;
        result.data[5] = (1.f - 2.f * (xx + zz)) * transform.scale.y;
        result.data[6] = (2.f * (yz + wx)) * transform.scale.y;
        result.data[8] = (2.f * (xz + wy)) * transform.scale.z;
        result.data[9] = (2.f * (yz - wx)) * transform.scale.z;
        result.data[10] = (1.f - 2.f * (xx + yy)) * transform.scale.z;
        result.data[12] = transform.position.x;
        result.data[13] = transform.position.y;
        result.data[14] = transform.position.z;
        result.data[15] = 1.f;
        return result;
    }
}

#endif
