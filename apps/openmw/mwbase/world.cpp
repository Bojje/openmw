#include "world.hpp"

#include <components/render/animation.hpp>

std::vector<Render::AnimationTextKey> MWBase::World::getNeutralAnimationTextKeys(
    const MWWorld::Ptr&, std::string_view, std::string_view, std::string_view) const
{
    return {};
}
