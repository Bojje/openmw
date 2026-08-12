#ifndef OPENMW_COMPONENTS_RENDER_IMAGEWRITER_H
#define OPENMW_COMPONENTS_RENDER_IMAGEWRITER_H

#include <array>
#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>

#include "texture.hpp"

namespace Render
{
    inline bool writeTga(const TextureData& image, const std::filesystem::path& path)
    {
        if (!image.valid() || image.width > std::numeric_limits<std::uint16_t>::max()
            || image.height > std::numeric_limits<std::uint16_t>::max())
            return false;

        const std::uint16_t width = static_cast<std::uint16_t>(image.width);
        const std::uint16_t height = static_cast<std::uint16_t>(image.height);
        const std::array<std::uint8_t, 18> header{ 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,
            static_cast<std::uint8_t>(width), static_cast<std::uint8_t>(width >> 8),
            static_cast<std::uint8_t>(height), static_cast<std::uint8_t>(height >> 8), 32, 0x28 };

        std::ofstream output(path, std::ios::binary);
        if (!output)
            return false;
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        for (std::size_t pixel = 0; pixel < image.pixels.size(); pixel += 4)
        {
            output.put(static_cast<char>(image.pixels[pixel + 2]));
            output.put(static_cast<char>(image.pixels[pixel + 1]));
            output.put(static_cast<char>(image.pixels[pixel + 0]));
            output.put(static_cast<char>(image.pixels[pixel + 3]));
        }
        return output.good();
    }

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
