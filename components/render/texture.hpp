#ifndef OPENMW_COMPONENTS_RENDER_TEXTURE_H
#define OPENMW_COMPONENTS_RENDER_TEXTURE_H

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <fstream>
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

    inline bool writePpm(const TextureData& image, const std::filesystem::path& path)
    {
        if (!image.valid())
            return false;

        std::ofstream output(path, std::ios::binary);
        if (!output)
            return false;
        output << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        for (std::size_t pixel = 0; pixel < image.pixels.size(); pixel += 4)
        {
            output.put(static_cast<char>(image.pixels[pixel + 0]));
            output.put(static_cast<char>(image.pixels[pixel + 1]));
            output.put(static_cast<char>(image.pixels[pixel + 2]));
        }
        return output.good();
    }

    using TextureResolver = std::function<std::shared_ptr<const TextureData>(std::string_view)>;
}

#endif
