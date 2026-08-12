#include "png.hpp"

#include <csetjmp>
#include <cstdint>

#include <png.h>

namespace
{
    struct PngOutputContext
    {
        std::vector<char>* output;
    };

    void writePngData(png_structp png, png_bytep data, png_size_t size)
    {
        auto* const context = static_cast<PngOutputContext*>(png_get_io_ptr(png));
        try
        {
            context->output->insert(context->output->end(), reinterpret_cast<const char*>(data),
                reinterpret_cast<const char*>(data) + size);
        }
        catch (...)
        {
            png_error(png, "PNG output allocation failed");
        }
    }

    void flushPngData(png_structp)
    {
    }
}

bool Render::writePng(const TextureData& image, std::vector<char>& output)
{
    output.clear();
    if (!image.valid())
        return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png)
        return false;
    png_infop info = png_create_info_struct(png);
    if (!info)
    {
        png_destroy_write_struct(&png, nullptr);
        return false;
    }

    if (setjmp(png_jmpbuf(png)) != 0)
    {
        png_destroy_write_struct(&png, &info);
        output.clear();
        return false;
    }

    PngOutputContext context{ &output };
    png_set_write_fn(png, &context, writePngData, flushPngData);
    png_set_IHDR(png, info, image.width, image.height, 8, PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    for (std::uint32_t y = 0; y < image.height; ++y)
    {
        png_bytep row = const_cast<png_bytep>(image.pixels.data() + static_cast<std::size_t>(y) * image.width * 4);
        png_write_row(png, row);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return !output.empty();
}
