#include <limits>
#include <stdexcept>
#include <string>

#include <components/render/submission.hpp>
#include <components/render/world.hpp>

int main()
{
    Render::CellScene scene;
    scene.exterior = true;
    scene.gridX = 2;
    scene.gridY = -3;
    scene.objects.push_back({ 7, "meshes/test.nif", {}, false });

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
    world.removeCell(&emptyCellHandle);

    Render::ObjectTransform objectTransform;
    objectTransform.position.x = 4.f;
    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif", objectTransform, false);
    const Render::CellScene* firstCell = world.findCell(&firstCellHandle);
    if (firstCell == nullptr || firstCell->name != "first" || firstCell->objects.size() != 1
        || firstCell->objects.front().visible)
        throw std::runtime_error("renderer-neutral world scene failed to record an object");

    objectTransform.position.x = 8.f;
    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/updated.nif", objectTransform, true);
    Render::WorldObject* recorded = world.findObject(&objectHandle);
    if (recorded == nullptr || recorded->model != "meshes/updated.nif" || !recorded->visible
        || recorded->transform.position.x != 8.f)
        throw std::runtime_error("renderer-neutral world scene failed to update an object");

    if (!world.updateObjectCell(&objectHandle, &updatedObjectHandle, &secondCellHandle, false, 0, 0, "second")
        || world.findObject(&objectHandle) != nullptr || world.findObject(&updatedObjectHandle) == nullptr
        || world.findCell(&firstCellHandle)->objects.size() != 0)
        throw std::runtime_error("renderer-neutral world scene failed to move an object");

    int dynamicObjectHandle = 0;
    world.recordObject(&dynamicObjectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/animated.nif",
        objectTransform, true, {}, true);
    if (world.findObject(&dynamicObjectHandle) == nullptr || !world.findObject(&dynamicObjectHandle)->dynamic)
        throw std::runtime_error("renderer-neutral world scene failed to retain dynamic-object state");
    const auto dynamicObjects = world.dynamicObjectsInOrder();
    if (dynamicObjects.size() != 1 || dynamicObjects.front().model != "meshes/animated.nif"
        || !dynamicObjects.front().visible)
        throw std::runtime_error("renderer-neutral world scene failed dynamic-object handoff");

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
    if (world.findObject(&updatedObjectHandle) != nullptr || world.findCell(&secondCellHandle) != nullptr
        || world.cellsInOrder().size() != 3 || world.cellsInOrder()[1] != world.findCell(&fourthCellHandle)
        || world.cellsInOrder()[2] != world.findCell(&thirdCellHandle))
        throw std::runtime_error("renderer-neutral world scene failed cell removal");

    world.clear();
    if (world.findCell(&firstCellHandle) != nullptr || world.findObject(&objectHandle) != nullptr
        || !world.cellsInOrder().empty())
        throw std::runtime_error("renderer-neutral world scene failed world reset");

    world.recordObject(&objectHandle, &firstCellHandle, true, 1, 2, "first", "meshes/first.nif", objectTransform, true);
    if (world.findObject(&objectHandle) == nullptr || world.findObject(&objectHandle)->id != 1)
        throw std::runtime_error("renderer-neutral world scene did not reset object identity");

    Render::SceneData aggregateScene = {};
    Render::MeshInstance aggregateMesh;
    aggregateMesh.mesh.vertices.resize(3);
    aggregateMesh.mesh.indices = { 0, 1, 2 };
    const Render::SceneSubmission aggregate = Render::collectSceneSubmission(world, aggregateScene, "",
        [&](std::string_view model) -> std::vector<Render::MeshInstance> {
            if (model != "meshes/first.nif")
                throw std::runtime_error("scene submission collector resolved an unexpected model");
            return { aggregateMesh };
        }, false);
    if (aggregate.meshes.size() != 1 || aggregate.dynamicObjects.size() != 0 || !aggregate.valid())
        throw std::runtime_error("renderer-neutral scene submission collector lost world state");

    Render::SceneSubmission submission;
    submission.scene.ambientColor = { 0.2f, 0.3f, 0.4f, 1.f };
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
        || !submission.valid())
        throw std::runtime_error("renderer-neutral scene submission failed resource handoff");

    submission.dynamicObjects.push_back({ 17, "meshes/animated.nif", objectTransform, true, true });
    if (submission.dynamicObjects.size() != 1 || !submission.dynamicObjects.front().dynamic
        || submission.dynamicObjects.front().model != "meshes/animated.nif" || !submission.valid())
        throw std::runtime_error("renderer-neutral scene submission lost dynamic records");

    submission.dynamicObjects.front().dynamic = false;
    if (submission.valid())
        throw std::runtime_error("renderer-neutral scene submission accepted a static dynamic record");
    submission.dynamicObjects.front().dynamic = true;

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
