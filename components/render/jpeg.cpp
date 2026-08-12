#include "jpeg.hpp"

#include <algorithm>
#include <csetjmp>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

#include <jpeglib.h>

namespace
{
    struct JpegErrorContext
    {
        jpeg_error_mgr manager;
        jmp_buf jump;
    };

    void jpegErrorExit(j_common_ptr cinfo)
    {
        auto* const error = reinterpret_cast<JpegErrorContext*>(cinfo->err);
        longjmp(error->jump, 1);
    }
}

bool Render::writeJpeg(const TextureData& image, std::vector<char>& output, int quality)
{
    output.clear();
    if (!image.valid() || image.width > std::numeric_limits<std::size_t>::max() / 3)
        return false;

    const std::size_t rowBytes = static_cast<std::size_t>(image.width) * 3;
    JSAMPLE* row = new (std::nothrow) JSAMPLE[rowBytes];
    if (!row)
        return false;

    jpeg_compress_struct jpeg{};
    JpegErrorContext error{};
    jpeg.err = jpeg_std_error(&error.manager);
    error.manager.error_exit = jpegErrorExit;
    unsigned char* encoded = nullptr;
    unsigned long encodedSize = 0;
    bool created = false;
    if (setjmp(error.jump) != 0)
    {
        delete[] row;
        if (created)
            jpeg_destroy_compress(&jpeg);
        std::free(encoded);
        return false;
    }

    jpeg_create_compress(&jpeg);
    created = true;
    jpeg_mem_dest(&jpeg, &encoded, &encodedSize);
    jpeg.image_width = image.width;
    jpeg.image_height = image.height;
    jpeg.input_components = 3;
    jpeg.in_color_space = JCS_RGB;
    jpeg_set_defaults(&jpeg);
    jpeg_set_quality(&jpeg, std::clamp(quality, 0, 100), TRUE);
    jpeg_start_compress(&jpeg, TRUE);

    while (jpeg.next_scanline < jpeg.image_height)
    {
        const std::size_t sourceOffset = static_cast<std::size_t>(jpeg.next_scanline) * image.width * 4;
        for (std::uint32_t x = 0; x < image.width; ++x)
        {
            row[x * 3 + 0] = image.pixels[sourceOffset + x * 4 + 0];
            row[x * 3 + 1] = image.pixels[sourceOffset + x * 4 + 1];
            row[x * 3 + 2] = image.pixels[sourceOffset + x * 4 + 2];
        }
        JSAMPROW scanline = row;
        if (jpeg_write_scanlines(&jpeg, &scanline, 1) != 1)
        {
            delete[] row;
            jpeg_destroy_compress(&jpeg);
            std::free(encoded);
            return false;
        }
    }
    jpeg_finish_compress(&jpeg);
    jpeg_destroy_compress(&jpeg);
    delete[] row;

    try
    {
        output.assign(reinterpret_cast<const char*>(encoded),
            reinterpret_cast<const char*>(encoded) + encodedSize);
    }
    catch (...)
    {
        std::free(encoded);
        throw;
    }
    std::free(encoded);
    return !output.empty();
}
