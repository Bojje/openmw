#ifndef OPENMW_APPS_OPENMW_MWWORLD_POSITIONCELLGRID_H
#define OPENMW_APPS_OPENMW_MWWORLD_POSITIONCELLGRID_H

#include <array>

#include <components/render/scene.hpp>

namespace MWWorld
{
    struct PositionCellGrid
    {
        Render::Vec3 mPosition{};
        std::array<int, 4> mCellBounds{};
    };
}

#endif
