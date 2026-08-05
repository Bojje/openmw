#ifndef OPENMW_COMPONENTS_RENDER_IMAGECOMPARISON_H
#define OPENMW_COMPONENTS_RENDER_IMAGECOMPARISON_H

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "texture.hpp"

namespace Render
{
    struct ImageComparison
    {
        bool sameDimensions = false;
        std::size_t comparedPixels = 0;
        std::size_t differingPixels = 0;
        std::uint8_t maxChannelError = 0;
        std::uint64_t totalChannelError = 0;

        double meanChannelError() const
        {
            const std::size_t channelCount = comparedPixels * 4;
            return channelCount == 0 ? 0.0 : static_cast<double>(totalChannelError) / channelCount;
        }

        bool matches(std::uint8_t channelTolerance, std::size_t allowedDifferingPixels = 0) const
        {
            return sameDimensions && differingPixels <= allowedDifferingPixels && maxChannelError <= channelTolerance;
        }
    };

    // Compare tightly packed RGBA8 images without depending on a graphics API
    // or image-loading library. The differing-pixel count is based on the
    // per-channel tolerance, while the error metrics remain useful for tuning
    // a visual comparison threshold.
    inline ImageComparison compareImages(const TextureData& reference, const TextureData& candidate,
        std::uint8_t channelTolerance = 0)
    {
        ImageComparison result;
        result.sameDimensions = reference.valid() && candidate.valid() && reference.width == candidate.width
            && reference.height == candidate.height;
        if (!result.sameDimensions)
            return result;

        result.comparedPixels = static_cast<std::size_t>(reference.width) * reference.height;
        for (std::size_t pixel = 0; pixel < result.comparedPixels; ++pixel)
        {
            bool differs = false;
            for (std::size_t channel = 0; channel < 4; ++channel)
            {
                const std::uint8_t referenceValue = reference.pixels[pixel * 4 + channel];
                const std::uint8_t candidateValue = candidate.pixels[pixel * 4 + channel];
                const std::uint8_t error = referenceValue > candidateValue ? referenceValue - candidateValue
                                                                            : candidateValue - referenceValue;
                result.maxChannelError = std::max(result.maxChannelError, error);
                result.totalChannelError += error;
                differs = differs || error > channelTolerance;
            }
            if (differs)
                ++result.differingPixels;
        }
        return result;
    }
}

#endif
