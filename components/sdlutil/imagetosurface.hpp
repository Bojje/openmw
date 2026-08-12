#ifndef OPENMW_COMPONENTS_SDLUTIL_IMAGETOSURFACE_H
#define OPENMW_COMPONENTS_SDLUTIL_IMAGETOSURFACE_H

#include <memory>

struct SDL_Surface;

namespace Render
{
    struct TextureData;
}

namespace SDLUtil
{
    typedef std::unique_ptr<SDL_Surface, void (*)(SDL_Surface*)> SurfaceUniquePtr;

    /// Convert tightly packed RGBA8 data to an SDL_Surface.
    SurfaceUniquePtr imageToSurface(const Render::TextureData& image, bool flip = false);

}

#endif
