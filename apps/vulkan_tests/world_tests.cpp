#include <cassert>
#include <string>

#include <components/render/world.hpp>

int main()
{
    Render::CellScene scene;
    scene.exterior = true;
    scene.gridX = 2;
    scene.gridY = -3;
    scene.objects.push_back({ 7, "meshes/test.nif", {} });

    assert(scene.objects.size() == 1);
    assert(scene.objects.front().id == 7);
    assert(scene.objects.front().model == "meshes/test.nif");
    assert(scene.objects.front().transform.rotation.w == 1.f);
    assert(scene.objects.front().transform.scale.x == 1.f);
}
