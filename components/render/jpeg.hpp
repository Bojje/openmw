#ifndef OPENMW_COMPONENTS_RENDER_JPEG_H
#define OPENMW_COMPONENTS_RENDER_JPEG_H

#include <vector>

#include "texture.hpp"

namespace Render
{
    /// Encode a renderer-neutral RGBA8 image as a JPEG byte stream.
    /// Returns false when the image is invalid or the encoder rejects it.
    bool writeJpeg(const TextureData& image, std::vector<char>& output, int quality = 90);
}

#endif
