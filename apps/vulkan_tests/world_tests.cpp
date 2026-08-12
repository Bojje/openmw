#include <limits>
#include <cmath>
#include <stdexcept>
#include <string>

#include <components/render/submission.hpp>
#include <components/render/world.hpp>

int main()
{
    const Render::Quat identityRotation = Render::makeEulerRotation({ 0.f, 0.f, 0.f });
    if (std::abs(identityRotation.x) > 1e-6f || std::abs(identityRotation.y) > 1e-6f
        || std::abs(identityRotation.z) > 1e-6f || std::abs(identityRotation.w - 1.f) > 1e-6f)
        throw std::runtime_error("renderer-neutral Euler rotation did not produce identity");

    Render::CellScene scene;
    scene.exterior = true;
    scene.gridX = 2;
    scene.gridY = -3;
    scene.objects.push_back({ 7, "meshes/test.nif", {}, false, false, {}, {} });

    if (scene.objects.size() != 1 || scene.objects.front().id != 7 || scene.objects.front().model != "meshes/test.nif"
        || scene.objects.front().transform.rotation.w != 1.f || scene.objects.front().transform.scale.x != 1.f
        || scene.objects.front().visible)
        throw std::runtime_error("renderer-neutral cell scene stored invalid object data");

    const Render::Mat4 matrix = Render::makeObjectTransformMatrix(scene.objects.front().transform);
    if (matrix.data[12] != 0.f || matrix.data[15] != 1.f)
        throw std::runtime_error("renderer-neutral object transform matrix is invalid");

    if (scene.findObject(7) == nullptr || !scene.eraseObject(7) || scene.findObject(7) != nullptr)
        throw std::runtime_error("renderer-neutral cell scene failed object ownership operations");

    int objectHandle = 0;
    int updatedObjectHandle = 0;
    int firstCellHandle = 0;
    int secondCellHandle = 0;
    Render::WorldScene world;
    if (!world.sceneData().valid() || world.sceneData().view.data[0] != 1.f
        || world.sceneData().view.data[5] != 1.f || world.sceneData().view.data[10] != 1.f
        || world.sceneData().view.data[15] != 1.f)
        throw std::runtime_error("renderer-neutral world scene did not initialize a valid camera state");
    int emptyCellHandle = 0;
    world.recordCell(&emptyCellHandle, true, 9, 9, "empty");
    if (world.findCell(&emptyCellHandle) == nullptr || !world.findCell(&emptyCellHandle)->objects.empty())
        throw std::runtime_error("renderer-neutral world scene failed to record an empty cell");

    Render::TerrainTile terrainTile;
    terrainTile.lod = 0;
    terrainTile.size = 1.f;
    terrainTile.cellWorldSize = 1.f;
    terrainTile.blendmapScale = 1.f;
    world.setTerrainTiles(&emptyCellHandle, { terrainTile });
    if (world.findCell(&emptyCellHandle)->terrainTiles.size() != 1
        || world.findCell(&emptyCellHandle)->terrainTiles.front().lod != 0)
        throw std::runtime_error("renderer-neutral world scene failed to own terrain snapshots");

    int waterCellHandle = 0;
    world.recordCell(&waterCellHandle, true, 0, 1, "water", "Tamriel",
        Render::WaterSurface{ 0.f, 10.f, 10.f, 20.f, 4.f });
    const std::vector<Render::MeshInstance> waterMeshes = Render::collectWaterMeshes(world, "Tamriel");
    if (waterMeshes.size() != 1 || waterMeshes.front().mesh.vertices.size() != 4
        || waterMeshes.front().mesh.indices.size() != 6 || waterMeshes.front().mesh.vertices.front().position[2] != 4.f
        || !waterMeshes.front().mesh.material.alphaBlend)
        throw std::runtime_error("renderer-neutral world scene failed to emit a water surface");
    if (!world.updateWaterLevel(&waterCellHandle, 7.f)
        || Render::collectWaterMeshes(world, "Tamriel").front().mesh.vertices.front().position[2] != 7.f)
        throw std::runtime_error("renderer-neutral world scene failed to update a water surface level");
    world.setWaterEnabled(false);
    if (!Render::collectWaterMeshes(world, "Tamriel").empty())
        throw std::runtime_error("renderer-neutral world scene ignored water visibility state");
    world.setWaterEnabled(true);
    int invalidWaterCellHandle = 0;
    world.recordCell(&invalidWaterCellHandle, true, 0, 2, "invalid-water", "Tamriel",
        Render::WaterSurface{ 0.f, 0.f, 10.f, 20.f, 4.f });
    const Render::SceneSubmission invalidWaterSubmission = Render::collectSceneSubmission(world, Render::SceneData(),
        "Tamriel", [](std::string_view) { return std::vector<Render::MeshInstance>(); }, false);
    if (invalidWaterSubmission.invalidWaterSurfaces != 1 || invalidWaterSubmission.valid()
        || invalidWaterSubmission.validationError() != "invalid water surface")
        throw std::runtime_error("renderer-neutral submission accepted an invalid water surface");
    world.removeCell(&invalidWaterCellHandle);
    world.removeCell(&waterCellHandle);
    world.removeCell(&emptyCellHandle);

    Render::WeatherEffects weather;
    weather.enabled = true;
    weather.alpha = 0.5f;
    weather.diameter = 100.f;
    weather.minHeight = 10.f;
    weather.maxHeight = 30.f;
    weather.speed = 4.f;
    weather.maxParticles = 2;
    world.setWeatherEffects(weather);
    const std::vector<Render::MeshInstance> weatherMeshes = Render::collectWeatherMeshes(world, world.sceneData());
    if (weatherMeshes.size() != 2 || !weatherMeshes.front().mesh.material.alphaBlend
        || weatherMeshes.front().mesh.material.diffuse.w != 0.5f
        || weatherMeshes.front().mesh.vertices.size() != 4)
        throw std::runtime_error("renderer-neutral world scene failed to emit precipitation geometry");
    const Render::SceneSubmission weatherSubmission = Render::collectSceneSubmission(world, world.sceneData(), "",
        [](std::string_view) { return std::vector<Render::MeshInstance>(); }, false);
    if (weatherSubmission.meshes.size() != 2 || !weatherSubmission.valid())
        throw std::runtime_error("renderer-neutral scene submission lost precipitation geometry");
    world.updateEffects(1.f);
    if (world.weatherTime() != 1.f)
        throw std::runtime_error("renderer-neutral world scene failed to advance precipitation time");
    world.clearWeatherEffects();
    if (!Render::collectWeatherMeshes(world, world.sceneData()).empty())
        throw std::runtime_error("renderer-neutral world scene failed to clear precipitation geometry");

    int staticCellHandle = 0;
    Render::ObjectTransform staticTransform;
    staticTransform.position = { 3.f, 4.f, 5.f };
    world.recordStaticObject(&staticCellHandle, true, 4, 5, "static", "grass/test.nif", staticTransform, true,
        "Tamriel");
    const Render::CellScene* staticCell = world.findCell(&staticCellHandle);
    if (staticCell == nullptr || staticCell->objects.size() != 1 || staticCell->objects.front().dynamic
        || staticCell->objects.front().model != "grass/test.nif"
        || staticCell->objects.front().transform.position.z != 5.f)
        throw std::runtime_error("renderer-neutral world scene failed to record a cell-owned static instance");
    world.removeCell(&staticCellHandle);

    Render::ObjectTransform objectTransform;
    objectTransform.position.x = 4.f;
    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif", objectTransform, false);
    const Render::CellScene* firstCell = world.findCell(&firstCellHandle);
    if (firstCell == nullptr || firstCell->name != "first" || firstCell->objects.size() != 1
        || firstCell->objects.front().visible)
        throw std::runtime_error("renderer-neutral world scene failed to record an object");

    objectTransform.position.x = 8.f;
    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/updated.nif", objectTransform, true);
    const Render::WorldObject& recorded = world.findCell(&firstCellHandle)->objects.front();
    if (recorded.model != "meshes/updated.nif" || !recorded.visible
        || recorded.transform.position.x != 8.f)
        throw std::runtime_error("renderer-neutral world scene failed to update an object");

    if (!world.updateObjectCell(&objectHandle, &updatedObjectHandle, &secondCellHandle, false, 0, 0, "second")
        || world.findCell(&firstCellHandle)->objects.size() != 0
        || world.findCell(&secondCellHandle)->objects.size() != 1)
        throw std::runtime_error("renderer-neutral world scene failed to move an object");

    int dynamicObjectHandle = 0;
    world.recordObject(&dynamicObjectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/animated.nif",
        objectTransform, true, {}, true);
    if (world.findCell(&firstCellHandle)->objects.size() != 1 || !world.findCell(&firstCellHandle)->objects.front().dynamic)
        throw std::runtime_error("renderer-neutral world scene failed to retain dynamic-object state");
    const auto& dynamicObject = world.findCell(&firstCellHandle)->objects.front();
    if (world.findCell(&firstCellHandle)->objects.size() != 1 || dynamicObject.model != "meshes/animated.nif"
        || !dynamicObject.visible)
        throw std::runtime_error("renderer-neutral world scene failed dynamic-object handoff");
    world.updateEffects(0.25f);
    if (world.findCell(&firstCellHandle)->objects.front().animationTime != 0.25f)
        throw std::runtime_error("renderer-neutral world scene did not advance dynamic animation time");
    Render::Mat4 dynamicBone = Render::identityMat4();
    dynamicBone.data[12] = 3.f;
    if (!world.updateObjectPose(&dynamicObjectHandle, { dynamicBone })
        || world.findCell(&firstCellHandle)->objects.front().boneMatrices.size() != 1
        || world.findCell(&firstCellHandle)->objects.front().boneMatrices.front().data[12] != 3.f)
        throw std::runtime_error("renderer-neutral world scene failed dynamic pose ownership");

    world.recordObject(&dynamicObjectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/replaced.nif",
        objectTransform, true, {}, true);
    if (!world.findCell(&firstCellHandle)->objects.front().boneMatrices.empty())
        throw std::runtime_error("renderer-neutral world scene retained a pose for a replaced dynamic model");

    int thirdCellHandle = 0;
    world.recordObject(&objectHandle, &thirdCellHandle, false, 3, 4, "third", "meshes/third.nif", objectTransform, true);
    int fourthObjectHandle = 0;
    int fourthCellHandle = 0;
    world.recordObject(&fourthObjectHandle, &fourthCellHandle, false, -5, 1, "late", "meshes/late.nif", objectTransform,
        true);
    const auto orderedCells = world.cellsInOrder();
    if (orderedCells.size() != 4 || orderedCells[0] != world.findCell(&firstCellHandle)
        || orderedCells[1] != world.findCell(&fourthCellHandle)
        || orderedCells[2] != world.findCell(&secondCellHandle)
        || orderedCells[3] != world.findCell(&thirdCellHandle))
        throw std::runtime_error("renderer-neutral world scene lost deterministic cell order");

    world.removeCell(&secondCellHandle);
    if (world.findCell(&secondCellHandle) != nullptr
        || world.cellsInOrder().size() != 3 || world.cellsInOrder()[1] != world.findCell(&fourthCellHandle)
        || world.cellsInOrder()[2] != world.findCell(&thirdCellHandle))
        throw std::runtime_error("renderer-neutral world scene failed cell removal");

    world.clear();
    if (world.findCell(&firstCellHandle) != nullptr
        || !world.cellsInOrder().empty())
        throw std::runtime_error("renderer-neutral world scene failed world reset");

    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif", objectTransform, true);
    if (world.findCell(&firstCellHandle) == nullptr || world.findCell(&firstCellHandle)->objects.front().id != 1)
        throw std::runtime_error("renderer-neutral world scene did not reset object identity");

    if (!world.updateObjectPosition(&objectHandle, { 1.f, 2.f, 3.f })
        || !world.updateObjectRotation(&objectHandle, { 0.f, 0.f, 0.5f, 0.5f })
        || !world.updateObjectScale(&objectHandle, { 2.f, 3.f, 4.f }))
        throw std::runtime_error("renderer-neutral world scene rejected explicit transform updates");
    const Render::WorldObject& updated = world.findCell(&firstCellHandle)->objects.front();
    if (updated.transform.position.x != 1.f || updated.transform.position.y != 2.f
        || updated.transform.position.z != 3.f || updated.transform.rotation.z != 0.5f
        || updated.transform.scale.x != 2.f || updated.transform.scale.y != 3.f
        || updated.transform.scale.z != 4.f)
        throw std::runtime_error("renderer-neutral world scene lost explicit transform updates");

    world.setActiveWorldspace("active");
    if (world.activeWorldspace() != "active")
        throw std::runtime_error("renderer-neutral world scene did not retain active worldspace");
    Render::Mat4 cameraMatrix = {};
    cameraMatrix.data[0] = 1.f;
    cameraMatrix.data[5] = 1.f;
    cameraMatrix.data[10] = 1.f;
    cameraMatrix.data[12] = 4.f;
    cameraMatrix.data[15] = 1.f;
    world.sceneData().view = cameraMatrix;
    world.sceneData().projection = cameraMatrix;
    world.sceneData().viewInverse = Render::invertMat4(cameraMatrix);
    world.sceneData().projInverse = Render::invertMat4(cameraMatrix);
    if (world.sceneData().view.data[12] != 4.f || world.sceneData().viewInverse.data[12] != -4.f)
        throw std::runtime_error("renderer-neutral world scene did not retain camera matrices");
    world.clear();
    if (!world.activeWorldspace().empty() || !world.sceneData().valid()
        || world.sceneData().view.data[0] != 1.f || world.sceneData().view.data[5] != 1.f
        || world.sceneData().view.data[10] != 1.f || world.sceneData().view.data[15] != 1.f)
        throw std::runtime_error("renderer-neutral world scene did not reset active worldspace");
    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif", objectTransform, true);

    Render::TerrainTile neutralTerrain;
    neutralTerrain.size = 1.f;
    neutralTerrain.center = { 1.5f, 2.5f };
    neutralTerrain.cellWorldSize = 1.f;
    neutralTerrain.verticesPerSide = 2;
    neutralTerrain.vertices.resize(4);
    neutralTerrain.indices = { 0, 1, 2, 2, 1, 3 };
    neutralTerrain.layers.push_back({ .diffuseTexture = "textures/terrain.dds", .normalTexture = {},
        .specularTexture = {}, .parallax = false, .specular = false, .blendmap = {} });
    world.setTerrainTiles(&firstCellHandle, { neutralTerrain });

    Render::SceneData aggregateScene;
    Render::MeshInstance aggregateMesh = {};
    aggregateMesh.mesh.vertices.resize(3);
    aggregateMesh.mesh.indices = { 0, 1, 2 };
    Render::MeshInstance secondEffectMesh = aggregateMesh;
    secondEffectMesh.mesh.material.albedoTexture = "textures/effect-original.dds";
    if (!world.recordEffect("spark", "meshes/effect.nif", { 10.f, 11.f, 12.f }, 2.f, "textures/effect.dds", true, 2.f,
            true)
        || world.recordEffect("", "meshes/effect.nif", { 10.f, 11.f, 12.f }, 1.f))
        throw std::runtime_error("renderer-neutral world scene accepted an invalid or anonymous effect");
    const Render::SceneSubmission effectSubmission = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif" && model != "meshes/effect.nif")
                throw std::runtime_error("effect scene submission resolved an unexpected model");
            return model == "meshes/effect.nif"
                ? std::vector<Render::MeshInstance>{ aggregateMesh, secondEffectMesh }
                : std::vector<Render::MeshInstance>{ aggregateMesh };
        }, false);
    if (effectSubmission.meshes.size() != 1 || effectSubmission.effects.size() != 1
        || effectSubmission.effects.back().meshes.size() != 2
        || effectSubmission.effects.back().meshes.back().transform.data[0] != 2.f
        || effectSubmission.effects.back().meshes.front().mesh.material.albedoTexture != "textures/effect.dds"
        || effectSubmission.effects.back().meshes.front().mesh.material.albedoWrapU
        || effectSubmission.effects.back().meshes.front().mesh.material.albedoWrapV
        || effectSubmission.effects.back().meshes.back().mesh.material.albedoTexture
            != "textures/effect-original.dds"
        || !effectSubmission.effects.back().object.looping
        || effectSubmission.effects.back().object.animationDuration != 2.f
        || !effectSubmission.effects.back().object.magicVfx
        || !effectSubmission.valid())
        throw std::runtime_error("renderer-neutral scene submission lost an identified effect");
    if (!world.removeEffect("spark") || world.removeEffect("spark"))
        throw std::runtime_error("renderer-neutral world scene failed effect removal");
    if (!world.recordEffect("loop", "meshes/effect.nif", { 1.f, 2.f, 3.f }, 1.f, {}, true, 2.f)
        || world.effectsInOrder().size() != 1 || !world.effectsInOrder().front()->looping)
        throw std::runtime_error("renderer-neutral world scene failed to retain an identified effect");
    world.updateEffects(1.5f);
    if (world.effectsInOrder().front()->animationTime != 1.5f)
        throw std::runtime_error("renderer-neutral world scene failed to advance an effect");
    world.updateEffects(1.f);
    if (world.effectsInOrder().front()->animationTime != 0.5f)
        throw std::runtime_error("renderer-neutral world scene failed to loop an effect");
    if (!world.recordEffect("oneshot", "meshes/effect.nif", { 1.f, 2.f, 3.f }, 1.f, {}, false, 1.f))
        throw std::runtime_error("renderer-neutral world scene failed to record a one-shot effect");
    world.updateEffects(1.f);
    if (world.effectsInOrder().size() != 1 || !world.effectsInOrder().front()->looping
        || world.effectsInOrder().front()->animationTime != 1.5f)
        throw std::runtime_error("renderer-neutral world scene failed to remove a completed effect");
    world.clearEffects();
    if (!world.effectsInOrder().empty())
        throw std::runtime_error("renderer-neutral world scene failed to clear effects");
    const Render::SceneSubmission aggregate = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("scene submission collector resolved an unexpected model");
            return { aggregateMesh };
        }, true);
    if (aggregate.meshes.size() != 1 || !aggregate.effects.empty() || aggregate.dynamicMeshes.size() != 0
        || aggregate.terrainTiles.size() != 1
        || !aggregate.valid())
        throw std::runtime_error("renderer-neutral scene submission collector lost world state");

    Render::TerrainRegion terrainRegion;
    terrainRegion.minCellX = 0;
    terrainRegion.maxCellX = 1;
    terrainRegion.minCellY = 0;
    terrainRegion.maxCellY = 1;
    Render::TerrainTile regionTerrain = neutralTerrain;
    regionTerrain.size = 2.f;
    regionTerrain.center = { 1.f, 1.f };
    terrainRegion.lods = { regionTerrain };
    world.setTerrainRegions({ terrainRegion });
    const Render::SceneSubmission regional = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("region scene submission resolved an unexpected model");
            return { aggregateMesh };
        }, true);
    if (regional.terrainTiles.size() != 1 || regional.terrainTiles.front().center != regionTerrain.center
        || !regional.valid())
        throw std::runtime_error("renderer-neutral scene submission did not select a region terrain snapshot");
    Render::TerrainTile regionLodOne = regionTerrain;
    regionLodOne.lod = 1;
    Render::TerrainTile regionLodTwo = regionTerrain;
    regionLodTwo.lod = 2;
    Render::TerrainRegion detailedRegion = terrainRegion;
    detailedRegion.lods = { regionTerrain, regionLodOne, regionLodTwo };
    Render::TerrainRegion adjacentRegion = terrainRegion;
    adjacentRegion.minCellX = 2;
    adjacentRegion.maxCellX = 3;
    adjacentRegion.lods = { regionTerrain };
    adjacentRegion.lods.front().center = { 3.f, 1.f };
    aggregateScene.viewInverse.data[12] = 200.f;
    world.setTerrainRegions({ detailedRegion, adjacentRegion });
    const Render::SceneSubmission stitchedRegions = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("neighboring region test resolved an unexpected model");
            return { aggregateMesh };
        }, true);
    if (stitchedRegions.terrainTiles.size() != 2 || stitchedRegions.terrainTiles[0].lod != 1
        || stitchedRegions.terrainTiles[1].lod != 0)
        throw std::runtime_error("renderer-neutral terrain regions did not constrain neighboring LOD gaps");
    aggregateScene.viewInverse.data[12] = 0.f;
    terrainRegion.maxCellX = 2;
    world.setTerrainRegions({ terrainRegion });
    const Render::SceneSubmission malformedRegionFallback = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("malformed region fallback resolved an unexpected model");
            return { aggregateMesh };
        }, true);
    if (malformedRegionFallback.terrainTiles.size() != 1 || !malformedRegionFallback.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted malformed terrain-region metadata");
    world.setTerrainRegions({ Render::TerrainRegion{} });
    const Render::SceneSubmission regionFallback = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("region fallback resolved an unexpected model");
            return { aggregateMesh };
        }, true);
    if (regionFallback.terrainTiles.size() != 1 || regionFallback.terrainTiles.front().center != neutralTerrain.center
        || !regionFallback.valid())
        throw std::runtime_error("renderer-neutral scene submission did not fall back from invalid regions");
    world.setTerrainRegions({});

    int activeWorldspaceCellHandle = 0;
    int inactiveWorldspaceCellHandle = 0;
    int activeWorldspaceObjectHandle = 0;
    int inactiveWorldspaceObjectHandle = 0;
    world.recordCell(&activeWorldspaceCellHandle, true, 20, 20, "active-space", "space-a");
    world.recordCell(&inactiveWorldspaceCellHandle, true, 21, 21, "inactive-space", "space-b");
    world.recordObject(&activeWorldspaceObjectHandle, &activeWorldspaceCellHandle, true, 20, 20, "active-space",
        "meshes/active-space.nif", objectTransform, true, "space-a");
    world.recordObject(&inactiveWorldspaceObjectHandle, &inactiveWorldspaceCellHandle, true, 21, 21,
        "inactive-space", "meshes/inactive-space.nif", objectTransform, true, "space-b");
    world.setTerrainTiles(&activeWorldspaceCellHandle, { neutralTerrain });
    world.setTerrainTiles(&inactiveWorldspaceCellHandle, { neutralTerrain });
    const Render::SceneSubmission activeWorldspace = Render::collectSceneSubmission(world, aggregateScene, "space-a",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/active-space.nif")
                throw std::runtime_error("scene submission leaked an inactive worldspace model");
            return { aggregateMesh };
        }, true);
    if (activeWorldspace.meshes.size() != 1 || activeWorldspace.terrainTiles.size() != 1
        || activeWorldspace.unresolvedModels.size() != 0)
        throw std::runtime_error("renderer-neutral scene submission did not filter active worldspace");
    world.removeCell(&activeWorldspaceCellHandle);
    world.removeCell(&inactiveWorldspaceCellHandle);

    const Render::SceneSubmission unresolved = Render::collectSceneSubmission(world, aggregateScene, "",
        [](std::string_view) { return std::vector<Render::MeshInstance>(); }, false);
    if (unresolved.unresolvedModels.size() != 1 || unresolved.unresolvedModels.front() != "meshes/first.nif"
        || unresolved.valid())
        throw std::runtime_error("renderer-neutral scene submission hid an unresolved visible model");

    Render::MeshInstance emptyGeometry = aggregateMesh;
    emptyGeometry.mesh.vertices.clear();
    emptyGeometry.mesh.indices.clear();
    Render::SceneSubmission malformedEmptyGeometry;
    malformedEmptyGeometry.meshes.push_back(emptyGeometry);
    malformedEmptyGeometry.meshes.front().transform.data[0] = std::numeric_limits<float>::quiet_NaN();
    if (malformedEmptyGeometry.valid())
        throw std::runtime_error("renderer-neutral validation accepted an invalid empty mesh transform");

    const Render::SceneSubmission emptyGeometrySubmission = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("empty renderer-neutral geometry resolved an unexpected model");
            return { emptyGeometry };
        }, false);
    if (emptyGeometrySubmission.unresolvedModels.size() != 1 || emptyGeometrySubmission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted a visible empty mesh batch");

    int dynamicSubmissionHandle = 0;
    world.recordObject(&dynamicSubmissionHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif",
        objectTransform, true, {}, true);
    if (!world.updateObjectPose(&dynamicSubmissionHandle, { dynamicBone }))
        throw std::runtime_error("renderer-neutral dynamic submission rejected a live pose update");
    aggregateMesh.mesh.material.albedoTexture = "textures/dynamic.dds";
    const Render::SceneSubmission dynamicSubmission = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("dynamic scene submission resolved an unexpected model");
            return { aggregateMesh };
        }, false);
    if (dynamicSubmission.dynamicMeshes.size() != 1
        || dynamicSubmission.dynamicMeshes.front().meshes.size() != 1
        || dynamicSubmission.dynamicMeshes.front().boneMatrices.size() != 1
        || dynamicSubmission.dynamicMeshes.front().boneMatrices.front().data[12] != 3.f
        || dynamicSubmission.referencedTexturePaths().size() != 1
        || dynamicSubmission.referencedTexturePaths().front() != "textures/dynamic.dds" || !dynamicSubmission.valid())
        throw std::runtime_error("renderer-neutral dynamic mesh payload was not collected");
    if (Render::collectRasterDynamicMeshes(dynamicSubmission).size() != 1)
        throw std::runtime_error("renderer-neutral unskinned dynamic mesh was not selected for rasterization");

    Render::SceneSubmission invalidAlphaSubmission = dynamicSubmission;
    invalidAlphaSubmission.meshes.push_back(aggregateMesh);
    invalidAlphaSubmission.meshes.back().mesh.material.alphaTexture
        = std::make_shared<const Render::TextureData>();
    if (invalidAlphaSubmission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted an invalid alpha texture");

    auto dynamicSkinning = std::make_shared<Render::SkinningData>();
    dynamicSkinning->vertices.resize(3);
    for (Render::SkinVertex& vertex : dynamicSkinning->vertices)
        vertex.weights[0] = 1.f;
    dynamicSkinning->boneNames.push_back("Root Bone");
    dynamicSkinning->inverseBindMatrices.push_back(Render::identityMat4());
    Render::MeshInstance skinnedDynamicMesh = aggregateMesh;
    skinnedDynamicMesh.mesh.skinning = dynamicSkinning;
    Render::DynamicMeshSubmission posedDynamic;
    posedDynamic.object = dynamicSubmission.dynamicMeshes.front().object;
    posedDynamic.meshes.push_back(skinnedDynamicMesh);
    Render::Mat4 bone = Render::identityMat4();
    bone.data[12] = 2.f;
    posedDynamic.boneMatrices.push_back(bone);
    Render::SceneSubmission posedDynamicSubmission = dynamicSubmission;
    posedDynamicSubmission.dynamicMeshes = { std::move(posedDynamic) };
    const std::vector<Render::MeshInstance> rasterDynamic
        = Render::collectRasterDynamicMeshes(posedDynamicSubmission);
    if (rasterDynamic.size() != 1 || rasterDynamic.front().mesh.skinning
        || rasterDynamic.front().mesh.vertices.front().position[0] != 2.f)
        throw std::runtime_error("renderer-neutral posed dynamic mesh was not rasterized");

    posedDynamicSubmission.dynamicMeshes.front().boneMatrices.clear();
    if (!Render::collectRasterDynamicMeshes(posedDynamicSubmission).empty())
        throw std::runtime_error("renderer-neutral skinned dynamic mesh without a pose was rasterized");

    posedDynamicSubmission.dynamicMeshes.front().boneMatrices.push_back(bone);
    posedDynamicSubmission.dynamicMeshes.front().boneMatrices.front().data[0]
        = std::numeric_limits<float>::quiet_NaN();
    if (posedDynamicSubmission.valid())
        throw std::runtime_error("renderer-neutral dynamic submission accepted a non-finite bone pose");

    int hiddenDynamicHandle = 0;
    world.recordObject(&hiddenDynamicHandle, &firstCellHandle, true, 1, 2, "first", "meshes/missing.nif",
        objectTransform, false, {}, true);
    const Render::SceneSubmission hiddenDynamicSubmission = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("hidden dynamic scene submission resolved an invisible model");
            return { aggregateMesh };
        }, false);
    if (hiddenDynamicSubmission.dynamicMeshes.size() != 2
        || hiddenDynamicSubmission.dynamicMeshes.back().object.model != "meshes/missing.nif"
        || !hiddenDynamicSubmission.dynamicMeshes.back().meshes.empty() || !hiddenDynamicSubmission.valid())
        throw std::runtime_error("renderer-neutral dynamic visibility policy was not preserved");
    if (Render::collectRasterDynamicMeshes(hiddenDynamicSubmission).size() != 1)
        throw std::runtime_error("renderer-neutral hidden dynamic mesh was selected for rasterization");

    world.removeObject(&objectHandle);
    world.setTerrainTiles(&firstCellHandle, {});
    const Render::SceneSubmission removed = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) {
            if (model == "meshes/first.nif")
                return std::vector<Render::MeshInstance>{ aggregateMesh };
            return std::vector<Render::MeshInstance>();
        }, true);
    if (!removed.meshes.empty() || !removed.terrainTiles.empty() || !removed.unresolvedModels.empty()
        || !removed.valid())
        throw std::runtime_error("renderer-neutral scene submission retained removed static ownership");

    Render::SceneSubmission submission;
    submission.scene = Render::SceneData();
    submission.scene.ambientColor = { 0.2f, 0.3f, 0.4f, 1.f };
    if (submission.validationError() != "no texture resolver")
        throw std::runtime_error("renderer-neutral submission validation missed a missing resolver");
    bool resolverCalled = false;
    submission.textureResolver = [&resolverCalled](std::string_view path) {
        if (path != "textures/submission.dds")
            throw std::runtime_error("scene submission passed an unexpected texture path");
        resolverCalled = true;
        return std::make_shared<const Render::TextureData>(Render::TextureData{ .width = 1,
            .height = 1,
            .pixels = { 255, 128, 0, 255 } });
    };
    const auto resolvedTexture = submission.textureResolver("textures/submission.dds");
    if (!resolverCalled || !resolvedTexture || !resolvedTexture->valid() || submission.scene.ambientColor.y != 0.3f
        || !submission.valid() || !submission.validationError().empty())
        throw std::runtime_error("renderer-neutral scene submission failed resource handoff");

    submission.dynamicMeshes.push_back({ { 17, "meshes/animated.nif", objectTransform, true, true, {}, {} }, {}, {} });
    if (submission.dynamicMeshes.size() != 1 || !submission.dynamicMeshes.front().object.dynamic
        || submission.dynamicMeshes.front().object.model != "meshes/animated.nif" || !submission.valid())
        throw std::runtime_error("renderer-neutral scene submission lost dynamic records");

    submission.dynamicMeshes.front().object.dynamic = false;
    if (submission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted a static dynamic record");
    submission.dynamicMeshes.front().object.dynamic = true;

    Render::MeshInstance malformedMesh;
    malformedMesh.mesh.vertices.resize(1);
    malformedMesh.mesh.indices.push_back(1);
    submission.meshes.push_back(std::move(malformedMesh));
    if (submission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted an invalid mesh index");
    submission.meshes.clear();

    submission.scene.view.data[0] = std::numeric_limits<float>::quiet_NaN();
    if (submission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted a non-finite scene matrix");
    submission.scene.view.data[0] = 0.f;

    Render::TerrainTile malformedTerrain;
    malformedTerrain.size = 1.f;
    submission.terrainTiles.push_back(std::move(malformedTerrain));
    if (submission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted an invalid terrain tile");
}
