#ifndef OPENMW_COMPONENTS_RENDER_PNG_H
#define OPENMW_COMPONENTS_RENDER_PNG_H

#include <vector>

#include "texture.hpp"

namespace Render
{
    /// Encode a renderer-neutral RGBA8 image as a PNG byte stream.
    /// Returns false when the image is invalid or the encoder rejects it.
    bool writePng(const TextureData& image, std::vector<char>& output);
}

#endif
