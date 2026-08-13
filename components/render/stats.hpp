#ifndef OPENMW_COMPONENTS_RENDER_STATS_HPP
#define OPENMW_COMPONENTS_RENDER_STATS_HPP

#include <string>
#include <string_view>
#include <unordered_map>

namespace Render
{
    /// Renderer-neutral profiling sink used by simulation and engine services.
    /// The OSG frame owner adapts this contract to osg::Stats; Vulkan can use
    /// the lightweight implementation without importing OSG statistics.
    class FrameStats
    {
    public:
        virtual ~FrameStats() = default;

        virtual bool collectStats(std::string_view group) const = 0;
        virtual void setAttribute(unsigned frameNumber, std::string_view name, double value) = 0;
    };

    class BasicFrameStats final : public FrameStats
    {
    public:
        bool collectStats(std::string_view /*group*/) const override { return mCollectStats; }
        void setAttribute(unsigned /*frameNumber*/, std::string_view name, double value) override
        {
            mAttributes[std::string(name)] = value;
        }

        void setCollectStats(bool collectStats) { mCollectStats = collectStats; }

    private:
        bool mCollectStats = false;
        std::unordered_map<std::string, double> mAttributes;
    };
}

#endif
