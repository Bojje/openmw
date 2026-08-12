#ifndef OPENMW_COMPONENTS_RENDER_IMAGEWRITER_H
#define OPENMW_COMPONENTS_RENDER_IMAGEWRITER_H

#include <filesystem>
#include <fstream>

#include "texture.hpp"

namespace Render
{
    // PPM remains the dependency-free fallback used by migration captures.
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
}

#endif
