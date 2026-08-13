#ifndef OPENMW_COMPONENTS_RENDER_SUBMISSION_H
#define OPENMW_COMPONENTS_RENDER_SUBMISSION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "mesh.hpp"
#include "scene.hpp"
#include "terrain.hpp"
#include "terrainpaging.hpp"
#include "texture.hpp"

namespace Render
{
    inline bool terrainRegionsAdjacent(const TerrainRegion& lhs, const TerrainRegion& rhs)
    {
        const bool horizontal = (lhs.maxCellX + 1 == rhs.minCellX || rhs.maxCellX + 1 == lhs.minCellX)
            && lhs.minCellY <= rhs.maxCellY && rhs.minCellY <= lhs.maxCellY;
        const bool vertical = (lhs.maxCellY + 1 == rhs.minCellY || rhs.maxCellY + 1 == lhs.minCellY)
            && lhs.minCellX <= rhs.maxCellX && rhs.minCellX <= lhs.maxCellX;
        return horizontal || vertical;
    }

    inline bool validMeshInstance(const MeshInstance& instance, bool allowEmptyIndices)
    {
        if (instance.mesh.indices.empty())
        {
            if (!allowEmptyIndices || !instance.mesh.vertices.empty())
                return false;
        }
        else if (instance.mesh.vertices.empty())
            return false;

        if (!Render::valid(instance.transform)
            || !Render::valid(instance.mesh.material.diffuse) || !Render::valid(instance.mesh.material.emissive)
            || !std::isfinite(instance.mesh.material.glossiness)
            || (instance.mesh.material.alphaTexture && !instance.mesh.material.alphaTexture->valid())
            || (instance.mesh.skinning && !instance.mesh.skinning->valid(instance.mesh.vertices.size())))
            return false;

        for (const MeshVertex& vertex : instance.mesh.vertices)
        {
            for (const float value : vertex.position)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.normal)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.texcoord)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.blendTexcoord)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.color)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.material)
                if (!std::isfinite(value))
                    return false;
            for (const float value : vertex.tangent)
                if (!std::isfinite(value))
                    return false;
        }
        return std::all_of(instance.mesh.indices.begin(), instance.mesh.indices.end(), [&](std::uint32_t index) {
            return index < instance.mesh.vertices.size();
        });
    }

    inline MeshInstance makeWaterSurfaceMesh(const WaterSurface& surface)
    {
        MeshInstance result;
        result.mesh.material.diffuse = { 0.08f, 0.2f, 0.28f, 0.78f };
        result.mesh.material.glossiness = 64.f;
        result.mesh.material.alphaBlend = true;
        result.mesh.material.doubleSided = true;
        result.mesh.material.waterSurface = true;
        result.mesh.vertices.resize(4);
        const std::array<Vec3, 4> positions = { Vec3{ surface.minX, surface.minY, surface.level },
            Vec3{ surface.maxX, surface.minY, surface.level }, Vec3{ surface.maxX, surface.maxY, surface.level },
            Vec3{ surface.minX, surface.maxY, surface.level } };
        const std::array<std::array<float, 2>, 4> texcoords = { std::array<float, 2>{ 0.f, 0.f },
            std::array<float, 2>{ 1.f, 0.f }, std::array<float, 2>{ 1.f, 1.f }, std::array<float, 2>{ 0.f, 1.f } };
        for (std::size_t i = 0; i < result.mesh.vertices.size(); ++i)
        {
            MeshVertex& vertex = result.mesh.vertices[i];
            vertex.position[0] = positions[i].x;
            vertex.position[1] = positions[i].y;
            vertex.position[2] = positions[i].z;
            vertex.normal[2] = 1.f;
            vertex.texcoord[0] = texcoords[i][0];
            vertex.texcoord[1] = texcoords[i][1];
            vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.f;
            vertex.tangent[3] = 1.f;
        }
        result.mesh.indices = { 0, 1, 2, 0, 2, 3 };
        return result;
    }

    inline std::vector<MeshInstance> collectWaterMeshes(const WorldScene& world, std::string_view worldspace = {})
    {
        std::vector<MeshInstance> result;
        if (!world.waterEnabled())
            return result;
        for (const CellScene* cell : world.cellsInOrder(worldspace))
        {
            if (cell->water && cell->water->valid())
                result.push_back(makeWaterSurfaceMesh(*cell->water));
            if (!cell->water || !cell->water->valid())
                continue;
            for (const WaterRipple& ripple : world.waterRipples())
            {
                if (ripple.position.x < cell->water->minX || ripple.position.x > cell->water->maxX
                    || ripple.position.y < cell->water->minY || ripple.position.y > cell->water->maxY)
                    continue;

                constexpr std::size_t segments = 16;
                constexpr float lifetime = 1.5f;
                const float progress = std::clamp(ripple.age / lifetime, 0.f, 1.f);
                const float innerRadius = ripple.size * (0.15f + progress * 0.65f);
                const float outerRadius = innerRadius + ripple.size * 0.12f;
                MeshInstance ring;
                ring.mesh.material.diffuse = { 0.65f, 0.85f, 1.f, 0.5f * (1.f - progress) };
                ring.mesh.material.alphaBlend = true;
                ring.mesh.material.doubleSided = true;
                ring.mesh.material.waterSurface = true;
                ring.mesh.vertices.reserve(segments * 2);
                ring.mesh.indices.reserve(segments * 6);
                for (std::size_t segment = 0; segment < segments; ++segment)
                {
                    const float angle = 2.f * 3.14159265358979323846f * static_cast<float>(segment) / segments;
                    const float cosine = std::cos(angle);
                    const float sine = std::sin(angle);
                    for (const float radius : { innerRadius, outerRadius })
                    {
                        MeshVertex vertex;
                        vertex.position[0] = ripple.position.x + cosine * radius;
                        vertex.position[1] = ripple.position.y + sine * radius;
                        vertex.position[2] = cell->water->level + 0.02f;
                        vertex.normal[2] = 1.f;
                        vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.f;
                        vertex.tangent[3] = 1.f;
                        ring.mesh.vertices.push_back(vertex);
                    }
                    const uint32_t current = static_cast<uint32_t>(segment * 2);
                    const uint32_t next = static_cast<uint32_t>(((segment + 1) % segments) * 2);
                    ring.mesh.indices.insert(ring.mesh.indices.end(), { current, next, current + 1, current + 1, next,
                        next + 1 });
                }
                result.push_back(std::move(ring));
            }
        }
        return result;
    }

    inline std::vector<MeshInstance> collectWeatherMeshes(const WorldScene& world, const SceneData& scene)
    {
        const WeatherEffects& weather = world.weatherEffects();
        if (!weather.enabled || !weather.valid() || weather.maxParticles <= 0 || !scene.valid())
            return {};

        const int particleCount = std::min(weather.maxParticles, 256);
        const float height = weather.maxHeight - weather.minHeight;
        const float lineLength = weather.snow ? 0.6f : std::clamp(height * 0.08f, 2.f, 32.f);
        const float halfWidth = weather.snow ? 0.12f : std::clamp(weather.diameter / 400.f, 0.02f, 0.2f);
        const float cameraX = scene.viewInverse.data[12];
        const float cameraY = scene.viewInverse.data[13];
        const float cameraZ = scene.viewInverse.data[14];
        float widthX = scene.viewInverse.data[0];
        float widthY = scene.viewInverse.data[1];
        const float widthLength = std::hypot(widthX, widthY);
        if (widthLength > std::numeric_limits<float>::epsilon())
        {
            widthX /= widthLength;
            widthY /= widthLength;
        }
        else
        {
            widthX = 1.f;
            widthY = 0.f;
        }
        std::vector<MeshInstance> result;
        result.reserve(static_cast<std::size_t>(particleCount));

        for (int index = 0; index < particleCount; ++index)
        {
            const uint32_t seed = static_cast<uint32_t>(index) * 1664525u + 1013904223u;
            const float horizontalX = static_cast<float>((seed >> 8) & 0xffffu) / 65535.f - 0.5f;
            const float horizontalY = static_cast<float>((seed >> 24) & 0xffu) / 255.f - 0.5f;
            const float heightFraction = static_cast<float>((seed >> 16) & 0xffffu) / 65535.f;
            const float fallingDistance = std::fmod(
                heightFraction * height + world.weatherTime() * weather.speed, height);
            const float x = cameraX + horizontalX * weather.diameter;
            const float y = cameraY + horizontalY * weather.diameter;
            const float z = cameraZ + weather.minHeight + fallingDistance;
            const Vec3 fallDirection = weather.fallDirection;
            const float fallLength = std::sqrt(
                fallDirection.x * fallDirection.x + fallDirection.y * fallDirection.y + fallDirection.z * fallDirection.z);
            const Vec3 fallOffset{ fallDirection.x * lineLength / fallLength,
                fallDirection.y * lineLength / fallLength, fallDirection.z * lineLength / fallLength };

            MeshInstance instance;
            instance.mesh.material.diffuse = { 1.f, 1.f, 1.f, weather.alpha };
            instance.mesh.material.alphaBlend = true;
            instance.mesh.material.doubleSided = true;
            instance.mesh.vertices.resize(4);
            const std::array<Vec3, 4> positions = { Vec3{ x - widthX * halfWidth, y - widthY * halfWidth, z },
                Vec3{ x + widthX * halfWidth, y + widthY * halfWidth, z },
                Vec3{ x + widthX * halfWidth + fallOffset.x, y + widthY * halfWidth + fallOffset.y,
                    z + fallOffset.z },
                Vec3{ x - widthX * halfWidth + fallOffset.x, y - widthY * halfWidth + fallOffset.y,
                    z + fallOffset.z } };
            for (std::size_t vertexIndex = 0; vertexIndex < positions.size(); ++vertexIndex)
            {
                MeshVertex& vertex = instance.mesh.vertices[vertexIndex];
                vertex.position[0] = positions[vertexIndex].x;
                vertex.position[1] = positions[vertexIndex].y;
                vertex.position[2] = positions[vertexIndex].z;
                vertex.normal[0] = widthY;
                vertex.normal[1] = -widthX;
                vertex.texcoord[0] = vertexIndex == 1 || vertexIndex == 2 ? 1.f : 0.f;
                vertex.texcoord[1] = vertexIndex >= 2 ? 1.f : 0.f;
                vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 1.f;
                vertex.tangent[3] = 1.f;
            }
            instance.mesh.indices = { 0, 1, 2, 0, 2, 3 };
            result.push_back(std::move(instance));
        }
        return result;
    }

    struct EffectMeshSubmission
    {
        WorldObject object;
        std::vector<MeshInstance> meshes;
    };

    inline void bakeMeshBindPose(MeshInstance& instance)
    {
        if (!instance.mesh.skinning || !instance.mesh.skinning->valid(instance.mesh.vertices.size()))
            return;

        std::vector<Mat4> bindPose;
        bindPose.reserve(instance.mesh.skinning->inverseBindMatrices.size());
        for (const Mat4& inverseBind : instance.mesh.skinning->inverseBindMatrices)
            bindPose.push_back(invertMat4(inverseBind));
        instance.mesh = skinMesh(instance.mesh, bindPose);
    }

    template <class ResolveMeshes>
    void collectEffectMeshes(const WorldScene& world, ResolveMeshes&& resolveMeshes,
        std::vector<EffectMeshSubmission>& result, std::vector<std::string>& unresolvedModels,
        bool includeBindPose = true)
    {
        for (const WorldObject* effect : world.effectsInOrder())
        {
            const std::vector<MeshInstance> resolvedMeshes = resolveMeshes(effect->model);
            const bool hasGeometry = std::any_of(resolvedMeshes.begin(), resolvedMeshes.end(), hasRenderableGeometry);
            if (!hasGeometry)
                unresolvedModels.push_back(effect->model);
            EffectMeshSubmission submission;
            submission.object = *effect;
            bool textureOverrideApplied = false;
            for (const MeshInstance& mesh : resolvedMeshes)
            {
                MeshInstance instance = transformMeshInstance(*effect, mesh);
                if (includeBindPose)
                    bakeMeshBindPose(instance);
                if (!effect->textureOverride.empty() && (!effect->magicVfx || !textureOverrideApplied))
                {
                    instance.mesh.material.albedoTexture = effect->textureOverride;
                    instance.mesh.material.albedoWrapU = false;
                    instance.mesh.material.albedoWrapV = false;
                    textureOverrideApplied = true;
                }
                submission.meshes.push_back(std::move(instance));
            }
            result.push_back(std::move(submission));
        }
    }

    struct DynamicMeshSubmission
    {
        WorldObject object;
        std::vector<MeshInstance> meshes;
        // A producer that owns animation may provide the current pose here.
        // Keeping the pose beside the dynamic record lets a backend consume
        // skinned meshes without borrowing an animation or scene-graph type.
        std::vector<Mat4> boneMatrices;
    };

    // One backend-neutral frame submission. Resource resolution remains a
    // callback because resource ownership belongs to the game/resource layer;
    // the renderer receives no OSG scene objects.
    struct SceneSubmission
    {
        SceneData scene;
        std::vector<MeshInstance> meshes;
        // Effects remain separate from ordinary world meshes so loop/lifetime
        // metadata crosses the backend boundary without duplicating draws.
        std::vector<EffectMeshSubmission> effects;
        std::vector<TerrainTile> terrainTiles;
        std::vector<std::string> unresolvedModels;
        std::size_t invalidWaterSurfaces = 0;
        // Dynamic records and their resolved meshes cross the frame boundary
        // together, so a future animation backend can consume skinning data
        // without borrowing WorldScene storage or re-resolving assets.
        std::vector<DynamicMeshSubmission> dynamicMeshes;
        TextureResolver textureResolver;

        bool valid() const
        {
            if (!scene.valid())
                return false;
            if (!unresolvedModels.empty() || invalidWaterSurfaces != 0)
                return false;

            for (const MeshInstance& instance : meshes)
            {
                if (!validMeshInstance(instance, true))
                    return false;
            }

            for (const DynamicMeshSubmission& dynamic : dynamicMeshes)
            {
                if (!dynamic.object.dynamic || dynamic.object.model.empty() || !dynamic.object.transform.valid())
                    return false;
                if (!std::all_of(dynamic.boneMatrices.begin(), dynamic.boneMatrices.end(),
                        [](const Mat4& matrix) { return Render::valid(matrix); }))
                    return false;
                for (const MeshInstance& instance : dynamic.meshes)
                {
                    if (!validMeshInstance(instance, false))
                        return false;
                    if (instance.mesh.skinning && !dynamic.boneMatrices.empty()
                        && dynamic.boneMatrices.size() < instance.mesh.skinning->inverseBindMatrices.size())
                        return false;
                }
            }

            for (const EffectMeshSubmission& effect : effects)
            {
                if (effect.object.dynamic || effect.object.model.empty() || !effect.object.transform.valid()
                    || !std::isfinite(effect.object.animationDuration) || effect.object.animationDuration < 0.f
                    || !std::isfinite(effect.object.animationTime) || effect.object.animationTime < 0.f
                    || (effect.object.animationDuration > 0.f
                        && effect.object.animationTime >= effect.object.animationDuration))
                    return false;
                for (const MeshInstance& instance : effect.meshes)
                    if (!validMeshInstance(instance, false) || instance.mesh.skinning)
                        return false;
            }

            return std::all_of(terrainTiles.begin(), terrainTiles.end(), [](const TerrainTile& tile) {
                return tile.valid();
            });
        }

        std::vector<std::string> referencedTexturePaths() const
        {
            std::unordered_set<std::string> seen;
            std::vector<std::string> result;
            const auto add = [&](std::string_view path) {
                if (!path.empty() && seen.emplace(path).second)
                    result.emplace_back(path);
            };
            const auto addMesh = [&add](const MeshInstance& instance) {
                add(instance.mesh.material.albedoTexture);
                add(instance.mesh.material.normalTexture);
                add(instance.mesh.material.emissiveTexture);
                add(instance.mesh.material.specularTexture);
            };
            for (const MeshInstance& instance : meshes)
                addMesh(instance);
            for (const DynamicMeshSubmission& dynamic : dynamicMeshes)
                for (const MeshInstance& instance : dynamic.meshes)
                    addMesh(instance);
            for (const EffectMeshSubmission& effect : effects)
                for (const MeshInstance& instance : effect.meshes)
                    addMesh(instance);
            for (const TerrainTile& tile : terrainTiles)
                for (const TerrainLayer& layer : tile.layers)
                {
                    add(layer.diffuseTexture);
                    add(layer.normalTexture);
                    add(layer.specularTexture);
                }
            return result;
        }

        std::string validationError() const
        {
            if (invalidWaterSurfaces != 0)
                return "invalid water surface";
            if (!valid())
                return "invalid geometry";
            if (!textureResolver)
                return "no texture resolver";
            for (const std::string& path : referencedTexturePaths())
            {
                const std::shared_ptr<const TextureData> texture = textureResolver(path);
                if (!texture || !texture->valid())
                    return "invalid texture resource for '" + path + "'";
            }
            return {};
        }
    };

    inline std::vector<MeshInstance> collectRasterDynamicMeshes(const SceneSubmission& submission)
    {
        std::vector<MeshInstance> result;
        for (const DynamicMeshSubmission& dynamic : submission.dynamicMeshes)
        {
            if (!dynamic.object.visible || !dynamic.object.active)
                continue;
            for (const MeshInstance& instance : dynamic.meshes)
            {
                if (!instance.mesh.skinning)
                {
                    result.push_back(instance);
                    continue;
                }

                if (dynamic.boneMatrices.empty())
                    continue;

                MeshInstance posed = instance;
                posed.mesh = skinMesh(instance.mesh, dynamic.boneMatrices);
                result.push_back(std::move(posed));
            }
        }
        return result;
    }

    inline const SkinningData* findCompatibleSkinning(std::span<const MeshInstance> meshes)
    {
        const auto skinned = std::find_if(meshes.begin(), meshes.end(),
            [](const MeshInstance& mesh) {
                return mesh.mesh.skinning && !mesh.mesh.skinning->boneNames.empty();
            });
        if (skinned == meshes.end())
            return nullptr;

        const bool compatible = std::all_of(meshes.begin(), meshes.end(),
            [&](const MeshInstance& mesh) {
                return !mesh.mesh.skinning
                    || (!mesh.mesh.skinning->boneNames.empty()
                        && mesh.mesh.skinning->boneNames == skinned->mesh.skinning->boneNames);
            });
        return compatible ? skinned->mesh.skinning.get() : nullptr;
    }

    inline const SkinningData* findCompatibleSkinning(const DynamicMeshSubmission& dynamic)
    {
        return findCompatibleSkinning(dynamic.meshes);
    }

    inline const SkinningData* findCompatibleSkinning(const EffectMeshSubmission& effect)
    {
        return findCompatibleSkinning(effect.meshes);
    }

    inline void applyBindPose(DynamicMeshSubmission& dynamic)
    {
        if (!dynamic.boneMatrices.empty())
            return;

        const SkinningData* skinning = findCompatibleSkinning(dynamic);
        if (skinning == nullptr)
            return;

        dynamic.boneMatrices.reserve(skinning->inverseBindMatrices.size());
        for (const Mat4& inverseBind : skinning->inverseBindMatrices)
            dynamic.boneMatrices.push_back(invertMat4(inverseBind));
    }

    // Build the backend-neutral portion of a frame from the scene owner. The
    // resource resolver remains supplied by the game layer, while mesh and
    // terrain collection stay independent of any renderer implementation.
    template <class ResolveMeshes>
    SceneSubmission collectSceneSubmission(const WorldScene& world, const SceneData& scene,
        std::string_view worldspace, ResolveMeshes&& resolveMeshes, bool includeTerrain = true,
        bool includeBindPose = true, bool includeEffectBindPose = true)
    {
        SceneSubmission result;
        result.scene = scene;
        result.meshes = collectWorldMeshes(world, resolveMeshes, worldspace, &result.unresolvedModels);
        std::vector<MeshInstance> weatherMeshes = collectWeatherMeshes(world, scene);
        result.meshes.insert(result.meshes.end(), std::make_move_iterator(weatherMeshes.begin()),
            std::make_move_iterator(weatherMeshes.end()));
        for (const CellScene* cell : world.cellsInOrder(worldspace))
            if (cell->water && !cell->water->valid())
                ++result.invalidWaterSurfaces;
        std::vector<MeshInstance> waterMeshes = collectWaterMeshes(world, worldspace);
        result.meshes.insert(result.meshes.end(), std::make_move_iterator(waterMeshes.begin()),
            std::make_move_iterator(waterMeshes.end()));
        collectEffectMeshes(world, resolveMeshes, result.effects, result.unresolvedModels, includeEffectBindPose);
        for (const CellScene* cell : world.cellsInOrder(worldspace))
            for (const WorldObject& object : cell->objects)
            {
                if (!object.dynamic)
                    continue;
                DynamicMeshSubmission dynamic;
                dynamic.object = object;
                dynamic.boneMatrices = object.boneMatrices;
                if (object.visible)
                {
                    const std::vector<MeshInstance> resolvedMeshes = resolveMeshes(object.model);
                    const bool hasGeometry
                        = std::any_of(resolvedMeshes.begin(), resolvedMeshes.end(), hasRenderableGeometry);
                    if (!hasGeometry)
                        result.unresolvedModels.push_back(object.model);
                    for (const MeshInstance& mesh : resolvedMeshes)
                        dynamic.meshes.push_back(transformMeshInstance(object, mesh));
                }
                if (includeBindPose)
                    applyBindPose(dynamic);
                result.dynamicMeshes.push_back(std::move(dynamic));
            }

        if (includeTerrain)
        {
            const float cameraX = scene.viewInverse.data[12];
            const float cameraY = scene.viewInverse.data[13];
            const bool hasCompleteRegions = !world.terrainRegions().empty()
                && std::all_of(world.terrainRegions().begin(), world.terrainRegions().end(),
                    [](const TerrainRegion& region) { return region.valid(); });
            if (hasCompleteRegions)
            {
                std::vector<const TerrainTile*> selected;
                selected.reserve(world.terrainRegions().size());
                for (const TerrainRegion& region : world.terrainRegions())
                    selected.push_back(selectTerrainLod(region.lods, cameraX, cameraY));

                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (std::size_t i = 0; i < world.terrainRegions().size(); ++i)
                        for (std::size_t j = i + 1; j < world.terrainRegions().size(); ++j)
                        {
                            if (!selected[i] || !selected[j]
                                || !terrainRegionsAdjacent(world.terrainRegions()[i], world.terrainRegions()[j]))
                                continue;
                            if (std::abs(selected[i]->lod - selected[j]->lod) <= 1)
                                continue;

                            const std::size_t coarse = selected[i]->lod > selected[j]->lod ? i : j;
                            const int maximumLod = selected[coarse == i ? j : i]->lod + 1;
                            const TerrainTile* constrained
                                = selectTerrainLod(world.terrainRegions()[coarse].lods, cameraX, cameraY, maximumLod);
                            if (constrained != nullptr && constrained->lod != selected[coarse]->lod)
                            {
                                selected[coarse] = constrained;
                                changed = true;
                            }
                        }
                }

                for (const TerrainTile* tile : selected)
                    if (tile != nullptr)
                        result.terrainTiles.push_back(*tile);
            }
            else
            {
                for (const CellScene* cell : world.cellsInOrder(worldspace))
                {
                    if (!cell->exterior || cell->terrainTiles.empty())
                        continue;
                    if (const TerrainTile* selected = selectTerrainLod(cell->terrainTiles, cameraX, cameraY))
                        result.terrainTiles.push_back(*selected);
                }
            }
        }
        return result;
    }
}

#endif
