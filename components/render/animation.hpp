#ifndef OPENMW_COMPONENTS_RENDER_ANIMATION_H
#define OPENMW_COMPONENTS_RENDER_ANIMATION_H

#include <string>

namespace Render
{
    /// A renderer-neutral animation event. Times are in the source
    /// controller's coordinate space; resource owners may rebase them to a
    /// selected group segment before dispatch.
    struct AnimationTextKey
    {
        float time = 0.f;
        std::string event;
    };
}

#endif
