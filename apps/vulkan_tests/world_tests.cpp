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
    if (!resolverCalled || !resolvedTexture || !resolvedTexture->valid() || submission.scene.ambientColor.y != 0.3f)
        throw std::runtime_error("renderer-neutral scene submission failed resource handoff");
}
