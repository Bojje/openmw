#ifndef OPENMW_COMPONENTS_RENDER_TEXTURE_H
#define OPENMW_COMPONENTS_RENDER_TEXTURE_H

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace Render
{
    // Backend-neutral, tightly packed RGBA8 texture data. Resource systems own
    // decoding; render backends own upload and lifetime.
    struct TextureData
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<uint8_t> pixels;

        bool valid() const
        {
            return width > 0 && height > 0 && pixels.size() == static_cast<std::size_t>(width) * height * 4;
        }
    };

    using TextureResolver = std::function<std::shared_ptr<const TextureData>(std::string_view)>;
}

#endif
