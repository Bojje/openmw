#include <stdexcept>
#include <string>

#include <components/render/world.hpp>

int main()
{
    Render::CellScene scene;
    scene.exterior = true;
    scene.gridX = 2;
    scene.gridY = -3;
    scene.objects.push_back({ 7, "meshes/test.nif", {} });

    if (scene.objects.size() != 1 || scene.objects.front().id != 7 || scene.objects.front().model != "meshes/test.nif"
        || scene.objects.front().transform.rotation.w != 1.f || scene.objects.front().transform.scale.x != 1.f)
        throw std::runtime_error("renderer-neutral cell scene stored invalid object data");

    const Render::Mat4 transform = Render::makeObjectTransformMatrix(scene.objects.front().transform);
    if (transform.data[12] != 0.f || transform.data[15] != 1.f)
        throw std::runtime_error("renderer-neutral object transform matrix is invalid");
}
