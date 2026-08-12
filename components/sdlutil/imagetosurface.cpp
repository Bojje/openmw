#include "imagetosurface.hpp"

#include <SDL_surface.h>

#include <components/render/texture.hpp>

namespace SDLUtil
{

    SurfaceUniquePtr imageToSurface(const Render::TextureData& image, bool flip)
    {
        if (!image.valid())
            return { nullptr, SDL_FreeSurface };

        const int width = static_cast<int>(image.width);
        const int height = static_cast<int>(image.height);
        SDL_Surface* surface
            = SDL_CreateRGBSurface(0, width, height, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);

        for (int x = 0; x < width; ++x)
            for (int y = 0; y < height; ++y)
            {
                const int sourceY = flip ? ((height - 1) - y) : y;
                const std::size_t offset = (static_cast<std::size_t>(sourceY) * width + x) * 4;
                const Uint8* const pixel = image.pixels.data() + offset;
                Uint8* const destination
                    = static_cast<Uint8*>(surface->pixels) + y * surface->pitch + x * surface->format->BytesPerPixel;
                *reinterpret_cast<Uint32*>(destination)
                    = SDL_MapRGBA(surface->format, pixel[0], pixel[1], pixel[2], pixel[3]);
            }

        return SurfaceUniquePtr(surface, SDL_FreeSurface);
    }

}
