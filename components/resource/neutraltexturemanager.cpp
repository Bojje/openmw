#include "neutraltexturemanager.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <iterator>
#include <stdexcept>
#include <vector>

#include <components/files/istreamptr.hpp>
#include <components/vfs/manager.hpp>

// Decoding stays in the neutral resource layer so Vulkan never needs an OSG image adapter.
namespace
{
    using Bytes = std::vector<std::uint8_t>;

    std::uint16_t read16(const Bytes& data, std::size_t offset)
    {
        if (offset + 2 > data.size())
            throw std::runtime_error("truncated texture header");
        return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
    }

    std::uint32_t read32(const Bytes& data, std::size_t offset)
    {
        if (offset + 4 > data.size())
            throw std::runtime_error("truncated texture header");
        return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8)
            | (static_cast<std::uint32_t>(data[offset + 2]) << 16)
            | (static_cast<std::uint32_t>(data[offset + 3]) << 24);
    }

    std::uint8_t expandChannel(std::uint32_t value, std::uint32_t mask)
    {
        if (!mask)
            return 255;
        const unsigned shift = static_cast<unsigned>(std::countr_zero(mask));
        const std::uint32_t channel = (value & mask) >> shift;
        const std::uint32_t maximum = mask >> shift;
        return static_cast<std::uint8_t>((channel * 255u + maximum / 2u) / maximum);
    }

    void setPixel(Render::TextureData& result, std::uint32_t x, std::uint32_t y,
        std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
    {
        const std::size_t offset = (static_cast<std::size_t>(y) * result.width + x) * 4;
        result.pixels[offset + 0] = r;
        result.pixels[offset + 1] = g;
        result.pixels[offset + 2] = b;
        result.pixels[offset + 3] = a;
    }

    std::shared_ptr<const Render::TextureData> decodeTga(const Bytes& data)
    {
        if (data.size() < 18 || (data[2] != 2 && data[2] != 10) || data[1] != 0)
            return {};
        const std::uint32_t width = read16(data, 12);
        const std::uint32_t height = read16(data, 14);
        const unsigned bits = data[16];
        if (!width || !height || (bits != 24 && bits != 32))
            return {};

        const std::size_t pixelBytes = bits / 8;
        const std::size_t offset = 18 + data[0];
        if (offset > data.size())
            return {};
        auto result = std::make_shared<Render::TextureData>();
        result->width = width;
        result->height = height;
        result->pixels.resize(static_cast<std::size_t>(width) * height * 4);
        std::size_t cursor = offset;
        std::size_t pixel = 0;
        const bool topDown = (data[17] & 0x20) != 0;
        const bool rightToLeft = (data[17] & 0x10) != 0;
        const auto write = [&](const std::uint8_t* source) {
            const std::uint32_t sourceX = static_cast<std::uint32_t>(pixel % width);
            const std::uint32_t sourceY = static_cast<std::uint32_t>(pixel / width);
            const std::uint32_t x = rightToLeft ? width - sourceX - 1 : sourceX;
            const std::uint32_t y = topDown ? sourceY : height - sourceY - 1;
            setPixel(*result, x, y, source[2], source[1], source[0], bits == 32 ? source[3] : 255);
            ++pixel;
        };
        while (pixel < static_cast<std::size_t>(width) * height)
        {
            std::size_t count = 1;
            bool run = false;
            if (data[2] == 10)
            {
                if (cursor >= data.size())
                    return {};
                const std::uint8_t packet = data[cursor++];
                count = (packet & 0x7f) + 1;
                run = (packet & 0x80) != 0;
            }
            if (count > static_cast<std::size_t>(width) * height - pixel || cursor + pixelBytes > data.size())
                return {};
            std::array<std::uint8_t, 4> source = {};
            std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(cursor), pixelBytes, source.begin());
            cursor += pixelBytes;
            write(source.data());
            if (run)
            {
                for (std::size_t i = 1; i < count; ++i)
                    write(source.data());
            }
            else
            {
                for (std::size_t i = 1; i < count; ++i)
                {
                    if (cursor + pixelBytes > data.size())
                        return {};
                    std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(cursor), pixelBytes, source.begin());
                    cursor += pixelBytes;
                    write(source.data());
                }
            }
        }
        return result;
    }

    std::shared_ptr<const Render::TextureData> decodeBmp(const Bytes& data)
    {
        if (data.size() < 54 || data[0] != 'B' || data[1] != 'M')
            return {};
        const std::uint32_t offset = read32(data, 10);
        const std::int32_t width = static_cast<std::int32_t>(read32(data, 18));
        const std::int32_t signedHeight = static_cast<std::int32_t>(read32(data, 22));
        const std::uint16_t planes = read16(data, 26);
        const std::uint16_t bits = read16(data, 28);
        const std::uint32_t compression = read32(data, 30);
        if (width <= 0 || signedHeight == 0 || planes != 1 || (bits != 24 && bits != 32) || compression != 0)
            return {};
        const std::uint32_t height = static_cast<std::uint32_t>(signedHeight < 0 ? -signedHeight : signedHeight);
        const std::size_t bytesPerRow = ((static_cast<std::size_t>(width) * bits + 31) / 32) * 4;
        if (offset > data.size() || (bytesPerRow > 0 && height > (data.size() - offset) / bytesPerRow))
            return {};
        auto result = std::make_shared<Render::TextureData>();
        result->width = static_cast<std::uint32_t>(width);
        result->height = height;
        result->pixels.resize(static_cast<std::size_t>(width) * height * 4);
        const std::size_t pixelBytes = bits / 8;
        for (std::uint32_t y = 0; y < height; ++y)
        {
            const std::uint32_t sourceY = signedHeight < 0 ? y : height - y - 1;
            const std::size_t row = offset + static_cast<std::size_t>(sourceY) * bytesPerRow;
            for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(width); ++x)
            {
                const std::size_t source = row + static_cast<std::size_t>(x) * pixelBytes;
                setPixel(*result, x, y, data[source + 2], data[source + 1], data[source], bits == 32 ? data[source + 3] : 255);
            }
        }
        return result;
    }

    std::shared_ptr<const Render::TextureData> decodeDds(const Bytes& data)
    {
        if (data.size() < 128 || data[0] != 'D' || data[1] != 'D' || data[2] != 'S' || data[3] != ' ')
            return {};
        const std::uint32_t height = read32(data, 12);
        const std::uint32_t width = read32(data, 16);
        const std::uint32_t fourCC = read32(data, 84);
        if (!width || !height)
            return {};
        auto result = std::make_shared<Render::TextureData>();
        result->width = width;
        result->height = height;
        result->pixels.resize(static_cast<std::size_t>(width) * height * 4);
        const auto color565 = [](std::uint16_t color) {
            return std::array<std::uint8_t, 3>{ static_cast<std::uint8_t>(((color >> 11) & 31) * 255 / 31),
                static_cast<std::uint8_t>(((color >> 5) & 63) * 255 / 63),
                static_cast<std::uint8_t>((color & 31) * 255 / 31) };
        };
        if (fourCC == 0x31545844 || fourCC == 0x33545844 || fourCC == 0x35545844)
        {
            const std::size_t blockBytes = fourCC == 0x31545844 ? 8 : 16;
            const std::size_t blocksX = (width + 3) / 4;
            const std::size_t blocksY = (height + 3) / 4;
            if (blocksX > 0 && blocksY > (data.size() - 128) / (blocksX * blockBytes))
                return {};
            std::size_t cursor = 128;
            for (std::size_t by = 0; by < blocksY; ++by)
                for (std::size_t bx = 0; bx < blocksX; ++bx)
                {
                    const std::size_t colorOffset = cursor + (fourCC == 0x31545844 ? 0 : 8);
                    const std::uint16_t c0 = read16(data, colorOffset);
                    const std::uint16_t c1 = read16(data, colorOffset + 2);
                    const auto rgb0 = color565(c0);
                    const auto rgb1 = color565(c1);
                    std::array<std::array<std::uint8_t, 4>, 4> colors = {};
                    colors[0] = { rgb0[0], rgb0[1], rgb0[2], 255 };
                    colors[1] = { rgb1[0], rgb1[1], rgb1[2], 255 };
                    if (c0 > c1 || fourCC != 0x31545844)
                    {
                        colors[2] = { static_cast<std::uint8_t>((2 * rgb0[0] + rgb1[0]) / 3),
                            static_cast<std::uint8_t>((2 * rgb0[1] + rgb1[1]) / 3),
                            static_cast<std::uint8_t>((2 * rgb0[2] + rgb1[2]) / 3), 255 };
                        colors[3] = { static_cast<std::uint8_t>((rgb0[0] + 2 * rgb1[0]) / 3),
                            static_cast<std::uint8_t>((rgb0[1] + 2 * rgb1[1]) / 3),
                            static_cast<std::uint8_t>((rgb0[2] + 2 * rgb1[2]) / 3), 255 };
                    }
                    else
                        colors[3] = { 0, 0, 0, 0 };
                    std::array<std::uint8_t, 8> alphaValues = {};
                    if (fourCC == 0x33545844)
                    {
                        // DXT3 stores one four-bit alpha value per pixel in the
                        // first eight bytes of the block.
                    }
                    else if (fourCC == 0x35545844)
                    {
                        alphaValues[0] = data[cursor + 0];
                        alphaValues[1] = data[cursor + 1];
                        if (alphaValues[0] > alphaValues[1])
                        {
                            for (int i = 1; i <= 6; ++i)
                                alphaValues[i + 1] = static_cast<std::uint8_t>(((7 - i) * alphaValues[0] + i * alphaValues[1]) / 7);
                        }
                        else
                        {
                            for (int i = 1; i <= 4; ++i)
                                alphaValues[i + 1] = static_cast<std::uint8_t>(((5 - i) * alphaValues[0] + i * alphaValues[1]) / 5);
                            alphaValues[6] = 0;
                            alphaValues[7] = 255;
                        }
                    }
                    const std::uint32_t colorBits = read32(data, colorOffset + 4);
                    std::uint64_t alphaBits48 = 0;
                    if (fourCC == 0x35545844)
                        for (unsigned byte = 0; byte < 6; ++byte)
                            alphaBits48 |= static_cast<std::uint64_t>(data[cursor + 2 + byte]) << (8 * byte);
                    for (unsigned y = 0; y < 4; ++y)
                        for (unsigned x = 0; x < 4; ++x)
                        {
                            const std::size_t index = y * 4 + x;
                            const auto& color = colors[(colorBits >> (2 * index)) & 3];
                            std::uint8_t alpha = 255;
                            if (fourCC == 0x33545844)
                                alpha = static_cast<std::uint8_t>(((data[cursor + index / 2] >> ((index % 2) * 4)) & 15) * 17);
                            else if (fourCC == 0x35545844)
                                alpha = alphaValues[(alphaBits48 >> (3 * index)) & 7];
                            if (bx * 4 + x < width && by * 4 + y < height)
                                setPixel(*result, bx * 4 + x, by * 4 + y, color[0], color[1], color[2], alpha);
                        }
                    cursor += blockBytes;
                }
            return result;
        }
        if (fourCC != 0)
            return {};
        const std::uint32_t bits = read32(data, 88);
        const std::uint32_t redMask = read32(data, 92);
        const std::uint32_t greenMask = read32(data, 96);
        const std::uint32_t blueMask = read32(data, 100);
        const std::uint32_t alphaMask = read32(data, 104);
        if (bits != 24 && bits != 32)
            return {};
        const std::size_t pixelBytes = bits / 8;
        if (pixelBytes > 0 && static_cast<std::size_t>(width) * height > (data.size() - 128) / pixelBytes)
            return {};
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::size_t offset = 128 + (static_cast<std::size_t>(y) * width + x) * pixelBytes;
                const std::uint32_t value = bits == 32 ? read32(data, offset)
                                                       : read32(Bytes{ data[offset], data[offset + 1], data[offset + 2], 0 }, 0);
                setPixel(*result, x, y, expandChannel(value, redMask), expandChannel(value, greenMask),
                    expandChannel(value, blueMask), alphaMask ? expandChannel(value, alphaMask) : 255);
            }
        return result;
    }

    std::shared_ptr<const Render::TextureData> decode(const Bytes& data, std::string_view extension)
    {
        if (extension == "tga")
            return decodeTga(data);
        if (extension == "bmp")
            return decodeBmp(data);
        if (extension == "dds")
            return decodeDds(data);
        return {};
    }
}

namespace Resource
{
    NeutralTextureManager::NeutralTextureManager(const VFS::Manager* vfs)
        : mVfs(vfs)
    {
    }

    std::shared_ptr<const Render::TextureData> NeutralTextureManager::get(VFS::Path::NormalizedView path)
    {
        const std::string key(path.value());
        {
            std::lock_guard lock(mMutex);
            const auto found = mCache.find(key);
            if (found != mCache.end())
                return found->second;
        }
        std::shared_ptr<const Render::TextureData> result;
        try
        {
            const Files::IStreamPtr stream = mVfs->find(path);
            if (stream)
            {
                Bytes bytes((std::istreambuf_iterator<char>(*stream)), std::istreambuf_iterator<char>());
                const VFS::Path::Normalized normalized(path);
                result = decode(bytes, normalized.extension().value());
            }
        }
        catch (const std::exception&)
        {
            result.reset();
        }
        std::lock_guard lock(mMutex);
        mCache.emplace(key, result);
        return result;
    }

    void NeutralTextureManager::clearCache()
    {
        std::lock_guard lock(mMutex);
        mCache.clear();
    }
}
