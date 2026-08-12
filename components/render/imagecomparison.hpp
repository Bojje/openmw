#ifndef OPENMW_COMPONENTS_RENDER_IMAGECOMPARISON_H
#define OPENMW_COMPONENTS_RENDER_IMAGECOMPARISON_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "texture.hpp"

namespace Render
{
    inline std::optional<TextureData> readPpm(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::nullopt;

        std::string magic;
        uint32_t width = 0;
        uint32_t height = 0;
        unsigned int maxValue = 0;
        if (!(input >> magic >> width >> height >> maxValue) || magic != "P6" || width == 0 || height == 0
            || maxValue != 255)
            return std::nullopt;

        // Formatted extraction leaves the separator before the binary pixel
        // payload unread. PPM permits arbitrary whitespace, but consuming one
        // byte is sufficient for the writer above and avoids treating the
        // first pixel as a separator.
        input.get();

        const std::size_t pixelCount = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> rgb(pixelCount * 3);
        input.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        if (!input)
            return std::nullopt;

        TextureData result;
        result.width = width;
        result.height = height;
        result.pixels.resize(pixelCount * 4);
        for (std::size_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            result.pixels[pixel * 4 + 0] = rgb[pixel * 3 + 0];
            result.pixels[pixel * 4 + 1] = rgb[pixel * 3 + 1];
            result.pixels[pixel * 4 + 2] = rgb[pixel * 3 + 2];
            result.pixels[pixel * 4 + 3] = 255;
        }
        return result;
    }

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
