#ifndef OPENMW_COMPONENTS_RENDER_WORLD_H
#define OPENMW_COMPONENTS_RENDER_WORLD_H

#include <cstdint>
#include <string>
#include <vector>

#include "scene.hpp"

namespace Render
{
    struct ObjectTransform
    {
        Vec3 position{};
        Quat rotation{ 0.f, 0.f, 0.f, 1.f };
        Vec3 scale{ 1.f, 1.f, 1.f };
    };

    // Renderer-neutral state for one loaded world reference. The id is owned by
    // the scene bridge and is intentionally opaque to backends.
    struct WorldObject
    {
        uint64_t id = 0;
        std::string model;
        ObjectTransform transform;
    };

    // A cell snapshot is updated by the world lifecycle, not by a renderer.
    struct CellScene
    {
        bool exterior = false;
        int gridX = 0;
        int gridY = 0;
        std::vector<WorldObject> objects;
    };
}

#endif
