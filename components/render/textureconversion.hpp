#ifndef OPENMW_COMPONENTS_RENDER_TEXTURECONVERSION_H
#define OPENMW_COMPONENTS_RENDER_TEXTURECONVERSION_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "texture.hpp"

namespace Render
{
    // Convert a sampled four-channel floating-point image into the one
    // backend-neutral texture representation. Resource decoders provide the
    // sampler; this component owns size, finite-value, and quantization rules.
    template <class Sample>
    TextureData makeRgba8Texture(std::uint32_t width, std::uint32_t height, Sample&& sample)
    {
        TextureData result;
        if (width == 0 || height == 0
            || width > std::numeric_limits<std::size_t>::max() / height
            || static_cast<std::size_t>(width) * height > std::numeric_limits<std::size_t>::max() / 4)
            return result;

        result.width = width;
        result.height = height;
        result.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::array<float, 4> color = sample(x, y);
                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
                for (std::size_t channel = 0; channel < color.size(); ++channel)
                {
                    if (!std::isfinite(color[channel]))
                        return {};
                    result.pixels[offset + channel]
                        = static_cast<std::uint8_t>(std::clamp(color[channel], 0.f, 1.f) * 255.f + 0.5f);
                }
            }
        }
        return result;
    }
}

#endif
