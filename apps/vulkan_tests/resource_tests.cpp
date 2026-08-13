#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#include <components/files/collections.hpp>
#include <components/resource/neutraltexturemanager.hpp>
#include <components/resource/resourcesystem.hpp>
#ifdef OPENMW_NEUTRAL_JPEG
#include <components/render/jpeg.hpp>
#endif
#ifdef OPENMW_NEUTRAL_PNG
#include <components/render/png.hpp>
#endif
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

namespace
{
    void write32(std::array<std::uint8_t, 58>& data, std::size_t offset, std::uint32_t value)
    {
        data[offset + 0] = static_cast<std::uint8_t>(value);
        data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        data[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    }

    void write32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value)
    {
        data[offset + 0] = static_cast<std::uint8_t>(value);
        data[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        data[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        data[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    }

    void testNeutralBmpTexture()
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "openmw-neutral-texture-test";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "textures");
        std::array<std::uint8_t, 58> bmp = {};
        bmp[0] = 'B';
        bmp[1] = 'M';
        write32(bmp, 2, bmp.size());
        write32(bmp, 10, 54);
        write32(bmp, 14, 40);
        write32(bmp, 18, 1);
        write32(bmp, 22, 1);
        bmp[26] = 1;
        bmp[28] = 24;
        write32(bmp, 34, 4);
        // One bottom-up BGR pixel: red, followed by row padding.
        bmp[54] = 0;
        bmp[55] = 0;
        bmp[56] = 255;
        bmp[57] = 0;
        std::vector<std::uint8_t> indexedBmp(14 + 40 + 8 + 4, 0);
        indexedBmp[0] = 'B';
        indexedBmp[1] = 'M';
        write32(indexedBmp, 2, indexedBmp.size());
        write32(indexedBmp, 10, 62);
        write32(indexedBmp, 14, 40);
        write32(indexedBmp, 18, 1);
        write32(indexedBmp, 22, 1);
        indexedBmp[26] = 1;
        indexedBmp[28] = 8;
        write32(indexedBmp, 46, 2);
        // Palette entries are BGRA; the single pixel selects the red entry.
        indexedBmp[54 + 4 + 2] = 255;
        indexedBmp[62] = 1;
        indexedBmp[63] = 0;
        std::vector<std::uint8_t> packedBmp(14 + 40 + 8 + 4, 0);
        packedBmp[0] = 'B';
        packedBmp[1] = 'M';
        write32(packedBmp, 2, packedBmp.size());
        write32(packedBmp, 10, 62);
        write32(packedBmp, 14, 40);
        write32(packedBmp, 18, 1);
        write32(packedBmp, 22, 1);
        packedBmp[26] = 1;
        packedBmp[28] = 4;
        write32(packedBmp, 46, 2);
        packedBmp[54 + 4 + 2] = 255;
        packedBmp[62] = 0x10;
        std::vector<std::uint8_t> bitfieldBmp(14 + 40 + 12 + 4, 0);
        bitfieldBmp[0] = 'B';
        bitfieldBmp[1] = 'M';
        write32(bitfieldBmp, 2, bitfieldBmp.size());
        write32(bitfieldBmp, 10, 66);
        write32(bitfieldBmp, 14, 40);
        write32(bitfieldBmp, 18, 1);
        write32(bitfieldBmp, 22, 1);
        bitfieldBmp[26] = 1;
        bitfieldBmp[28] = 16;
        write32(bitfieldBmp, 30, 3); // BI_BITFIELDS
        write32(bitfieldBmp, 54, 0xf800);
        write32(bitfieldBmp, 58, 0x07e0);
        write32(bitfieldBmp, 62, 0x001f);
        bitfieldBmp[66] = 0x00;
        bitfieldBmp[67] = 0xf8; // RGB565 red
        std::vector<std::uint8_t> dds(144, 0);
        dds[0] = 'D';
        dds[1] = 'D';
        dds[2] = 'S';
        dds[3] = ' ';
        write32(dds, 4, 124);
        write32(dds, 12, 4);
        write32(dds, 16, 4);
        write32(dds, 76, 32);
        write32(dds, 84, 0x32495441); // ATI2 / BC5U
        dds[128] = 64;
        dds[129] = 192;
        dds[136] = 128;
        dds[137] = 128;
        dds[130] = 1; // The first pixel uses BC4 palette entry 1.
        dds[138] = 1;
        auto dxt2 = std::vector<std::uint8_t>(144, 0);
        dxt2[0] = 'D';
        dxt2[1] = 'D';
        dxt2[2] = 'S';
        dxt2[3] = ' ';
        write32(dxt2, 4, 124);
        write32(dxt2, 12, 4);
        write32(dxt2, 16, 4);
        write32(dxt2, 76, 32);
        write32(dxt2, 84, 0x32545844); // DXT2, the premultiplied DXT3 alias.
        std::fill(dxt2.begin() + 128, dxt2.begin() + 136, 0xff);
        dxt2[136] = 0x00;
        dxt2[137] = 0xf8; // RGB565 red.
        dxt2[138] = 0x00;
        dxt2[139] = 0x07; // RGB565 green.
        dxt2[128] = 0xf8; // First pixel uses a non-opaque DXT3 alpha nibble.
        auto dxt4 = dxt2;
        write32(dxt4, 84, 0x34545844); // DXT4, the premultiplied DXT5 alias.
        dxt4[128] = 0;
        dxt4[129] = 255; // First pixel uses the first DXT5 alpha endpoint.
        std::fill(dxt4.begin() + 130, dxt4.begin() + 136, 0);
        std::vector<std::uint8_t> tga(20, 0);
        tga[2] = 2;
        tga[12] = 1;
        tga[14] = 1;
        tga[16] = 16;
        tga[17] = 0x20;
        tga[18] = 0;
        tga[19] = 0x7c; // 16-bit true-color red (BGR5551), alpha ignored like OSG.
        std::vector<std::uint8_t> indexedTga(18 + 6 + 2, 0);
        indexedTga[1] = 1;
        indexedTga[2] = 9;
        indexedTga[5] = 2;
        indexedTga[7] = 24;
        indexedTga[12] = 1;
        indexedTga[14] = 1;
        indexedTga[16] = 8;
        indexedTga[17] = 0x20;
        indexedTga[18 + 3 + 2] = 255;
        indexedTga[24] = 0x80;
        indexedTga[25] = 1;
        std::vector<std::uint8_t> dx10Rgba(152, 0);
        dx10Rgba[0] = 'D';
        dx10Rgba[1] = 'D';
        dx10Rgba[2] = 'S';
        dx10Rgba[3] = ' ';
        write32(dx10Rgba, 4, 124);
        write32(dx10Rgba, 12, 1);
        write32(dx10Rgba, 16, 1);
        write32(dx10Rgba, 76, 32);
        write32(dx10Rgba, 84, 0x30315844); // DX10 extended header.
        write32(dx10Rgba, 128, 28); // DXGI_FORMAT_R8G8B8A8_UNORM.
        write32(dx10Rgba, 132, 3); // D3D10_RESOURCE_DIMENSION_TEXTURE2D.
        write32(dx10Rgba, 136, 1); // Array size.
        dx10Rgba[148] = 12;
        dx10Rgba[149] = 34;
        dx10Rgba[150] = 56;
        dx10Rgba[151] = 255;
        std::vector<std::uint8_t> rgb24Dds(132, 0);
        rgb24Dds[0] = 'D';
        rgb24Dds[1] = 'D';
        rgb24Dds[2] = 'S';
        rgb24Dds[3] = ' ';
        write32(rgb24Dds, 4, 124);
        write32(rgb24Dds, 12, 1);
        write32(rgb24Dds, 16, 1);
        write32(rgb24Dds, 20, 4); // One padded BGR24 row.
        write32(rgb24Dds, 76, 32);
        write32(rgb24Dds, 80, 0x40); // DDPF_RGB.
        write32(rgb24Dds, 88, 24);
        write32(rgb24Dds, 92, 0x00ff0000);
        write32(rgb24Dds, 96, 0x0000ff00);
        write32(rgb24Dds, 100, 0x000000ff);
        rgb24Dds[128] = 0;
        rgb24Dds[129] = 0;
        rgb24Dds[130] = 255; // BGR24 red.
        std::vector<std::uint8_t> ktx(72, 0);
        const std::array<std::uint8_t, 12> ktxIdentifier
            = { 0xab, 0x4b, 0x54, 0x58, 0x20, 0x31, 0x31, 0xbb, 0x0d, 0x0a, 0x1a, 0x0a };
        std::copy(ktxIdentifier.begin(), ktxIdentifier.end(), ktx.begin());
        write32(ktx, 12, 0x04030201); // Little-endian marker.
        write32(ktx, 16, 0x1401); // GL_UNSIGNED_BYTE.
        write32(ktx, 20, 1);
        write32(ktx, 24, 0x1908); // GL_RGBA.
        write32(ktx, 28, 0x8058); // GL_RGBA8.
        write32(ktx, 32, 0x1908);
        write32(ktx, 36, 1);
        write32(ktx, 40, 1);
        write32(ktx, 52, 1); // One cubemap face.
        write32(ktx, 56, 1); // One mip level.
        write32(ktx, 64, 4); // Image byte count.
        ktx[68] = 17;
        ktx[69] = 34;
        ktx[70] = 51;
        ktx[71] = 255;
        {
            std::ofstream output(root / "textures/test.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        }
        {
            std::ofstream output(root / "textures/indexed.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(indexedBmp.data()),
                static_cast<std::streamsize>(indexedBmp.size()));
        }
        {
            std::ofstream output(root / "textures/packed.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(packedBmp.data()),
                static_cast<std::streamsize>(packedBmp.size()));
        }
        {
            std::ofstream output(root / "textures/bitfield.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(bitfieldBmp.data()),
                static_cast<std::streamsize>(bitfieldBmp.size()));
        }
        {
            std::ofstream output(root / "textures/normal.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
        }
        {
            std::ofstream output(root / "textures/dxt2.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(dxt2.data()), static_cast<std::streamsize>(dxt2.size()));
        }
        {
            std::ofstream output(root / "textures/dxt4.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(dxt4.data()), static_cast<std::streamsize>(dxt4.size()));
        }
        {
            std::ofstream output(root / "textures/test.tga", std::ios::binary);
            output.write(reinterpret_cast<const char*>(tga.data()), static_cast<std::streamsize>(tga.size()));
        }
        {
            std::ofstream output(root / "textures/indexed.tga", std::ios::binary);
            output.write(reinterpret_cast<const char*>(indexedTga.data()),
                static_cast<std::streamsize>(indexedTga.size()));
        }
        {
            std::ofstream output(root / "textures/dx10.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(dx10Rgba.data()), static_cast<std::streamsize>(dx10Rgba.size()));
        }
        {
            std::ofstream output(root / "textures/rgb24.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(rgb24Dds.data()), static_cast<std::streamsize>(rgb24Dds.size()));
        }
        {
            std::ofstream output(root / "textures/test.ktx", std::ios::binary);
            output.write(reinterpret_cast<const char*>(ktx.data()), static_cast<std::streamsize>(ktx.size()));
        }

        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        VFS::Manager vfs;
        Files::Collections collections(Files::PathContainer{ root });
        VFS::registerArchives(&vfs, collections, {}, true, &encoder.getStatelessEncoder());
        Resource::ResourceSystem resources(
            &vfs, 1.0, &encoder.getStatelessEncoder(), Resource::ResourceSystem::Backend::Neutral);
        const auto texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.bmp"));
        if (!texture || texture->width != 1 || texture->height != 1
            || texture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral BMP texture decoding changed pixel data");
        const auto indexedTexture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/indexed.bmp"));
        if (!indexedTexture || indexedTexture->width != 1 || indexedTexture->height != 1
            || indexedTexture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral indexed BMP texture decoding changed pixel data");
        const auto packedTexture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/packed.bmp"));
        if (!packedTexture || packedTexture->width != 1 || packedTexture->height != 1
            || packedTexture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral packed BMP texture decoding changed pixel data");
        const auto bitfieldTexture
            = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/bitfield.bmp"));
        if (!bitfieldTexture || bitfieldTexture->width != 1 || bitfieldTexture->height != 1
            || bitfieldTexture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral 16-bit BMP texture decoding changed pixel data");
        const auto tgaTexture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.tga"));
        if (!tgaTexture || tgaTexture->width != 1 || tgaTexture->height != 1
            || tgaTexture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral 16-bit TGA texture decoding changed pixel data");
        const auto indexedTgaTexture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/indexed.tga"));
        if (!indexedTgaTexture || indexedTgaTexture->width != 1 || indexedTgaTexture->height != 1
            || indexedTgaTexture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral indexed TGA texture decoding changed pixel data");
        const auto normal = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/normal.dds"));
        if (!normal || normal->width != 4 || normal->height != 4 || normal->pixels.size() != 4 * 4 * 4
            || normal->pixels[0] < 190 || normal->pixels[0] > 194 || normal->pixels[1] < 126
            || normal->pixels[1] > 130 || normal->pixels[2] < 235 || normal->pixels[2] > 240
            || normal->pixels[3] != 255 || normal->pixels[4] < 62 || normal->pixels[4] > 66)
            throw std::runtime_error("neutral BC5 texture decoding did not reconstruct a normal");
        const auto dxt2Texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/dxt2.dds"));
        bool dxt2IsRed = dxt2Texture && dxt2Texture->width == 4 && dxt2Texture->height == 4
            && dxt2Texture->pixels.size() == 4 * 4 * 4;
        if (dxt2IsRed)
            for (std::size_t pixel = 0; pixel < 16; ++pixel)
                dxt2IsRed = dxt2Texture->pixels[pixel * 4] == 255 && dxt2Texture->pixels[pixel * 4 + 1] == 0
                    && dxt2Texture->pixels[pixel * 4 + 2] == 0
                    && dxt2Texture->pixels[pixel * 4 + 3] == (pixel == 0 ? 136 : 255);
        if (!dxt2IsRed)
            throw std::runtime_error("neutral DXT2 texture decoding did not preserve premultiplied DXT3 data");
        const auto dxt4Texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/dxt4.dds"));
        if (!dxt4Texture || dxt4Texture->width != 4 || dxt4Texture->height != 4
            || dxt4Texture->pixels[0] != 255 || dxt4Texture->pixels[1] != 0 || dxt4Texture->pixels[2] != 0
            || dxt4Texture->pixels[3] != 0)
            throw std::runtime_error("neutral DXT4 texture decoding did not preserve premultiplied DXT5 data");
        const auto dx10Texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/dx10.dds"));
        if (!dx10Texture || dx10Texture->width != 1 || dx10Texture->height != 1
            || dx10Texture->pixels != std::vector<std::uint8_t>({ 12, 34, 56, 255 }))
            throw std::runtime_error("neutral DX10 DDS texture decoding changed pixel data");
        const auto rgb24Texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/rgb24.dds"));
        if (!rgb24Texture || rgb24Texture->width != 1 || rgb24Texture->height != 1
            || rgb24Texture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral 24-bit DDS texture decoding changed pixel data");
        const auto ktxTexture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.ktx"));
        if (!ktxTexture || ktxTexture->width != 1 || ktxTexture->height != 1
            || ktxTexture->pixels != std::vector<std::uint8_t>({ 17, 34, 51, 255 }))
            throw std::runtime_error("neutral KTX texture decoding changed pixel data");
        const auto correctedTexture
            = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/normal.tga"));
        if (!correctedTexture || correctedTexture != normal)
            throw std::runtime_error("neutral texture resolution did not apply the legacy DDS fallback");
        auto oversized = dds;
        write32(oversized, 12, std::numeric_limits<std::uint32_t>::max());
        write32(oversized, 16, std::numeric_limits<std::uint32_t>::max());
        {
            std::ofstream output(root / "textures/normal.dds", std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(oversized.data()), static_cast<std::streamsize>(oversized.size()));
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/normal.dds")))
            throw std::runtime_error("oversized neutral BC5 texture unexpectedly allocated");
        {
            std::ofstream output(root / "textures/normal.dds", std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(dds.data()), 128);
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/normal.dds")))
            throw std::runtime_error("truncated neutral BC5 texture unexpectedly decoded");
        std::filesystem::remove_all(root, error);
    }

#ifdef OPENMW_NEUTRAL_PNG
    void testNeutralPngTexture()
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "openmw-neutral-png-test";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "textures", error);
        if (error)
            throw std::runtime_error("could not create neutral PNG test directory");

        Render::TextureData source{ .width = 1, .height = 1, .pixels = { 255, 0, 0, 255 } };
        std::vector<char> encoded;
        if (!Render::writePng(source, encoded))
            throw std::runtime_error("could not encode neutral PNG test file");
        {
            std::ofstream output(root / "textures/test.png", std::ios::binary);
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        }

        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        VFS::Manager vfs;
        Files::Collections collections(Files::PathContainer{ root });
        VFS::registerArchives(&vfs, collections, {}, true, &encoder.getStatelessEncoder());
        Resource::ResourceSystem resources(
            &vfs, 1.0, &encoder.getStatelessEncoder(), Resource::ResourceSystem::Backend::Neutral);
        const auto texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.png"));
        if (!texture || texture->width != 1 || texture->height != 1
            || texture->pixels != std::vector<std::uint8_t>({ 255, 0, 0, 255 }))
            throw std::runtime_error("neutral PNG texture decoding changed pixel data");

        {
            std::ofstream output(root / "textures/test.png", std::ios::binary | std::ios::trunc);
            output.write(encoded.data(), 32);
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.png")))
            throw std::runtime_error("malformed neutral PNG unexpectedly decoded");

        std::filesystem::remove_all(root, error);
    }
#endif

#ifdef OPENMW_NEUTRAL_JPEG
    void testNeutralJpegTexture()
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "openmw-neutral-jpeg-test";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "textures", error);
        if (error)
            throw std::runtime_error("could not create neutral JPEG test directory");
        Render::TextureData source{ .width = 1, .height = 1, .pixels = { 127, 127, 127, 255 } };
        std::vector<char> encoded;
        if (!Render::writeJpeg(source, encoded))
            throw std::runtime_error("could not encode neutral JPEG test file");
        {
            std::ofstream output(root / "textures/test.jpg", std::ios::binary);
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
        }

        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        VFS::Manager vfs;
        Files::Collections collections(Files::PathContainer{ root });
        VFS::registerArchives(&vfs, collections, {}, true, &encoder.getStatelessEncoder());
        Resource::ResourceSystem resources(
            &vfs, 1.0, &encoder.getStatelessEncoder(), Resource::ResourceSystem::Backend::Neutral);
        const auto texture = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.jpg"));
        if (!texture || texture->width != 1 || texture->height != 1 || texture->pixels.size() != 4
            || texture->pixels[0] < 115 || texture->pixels[0] > 140 || texture->pixels[1] < 115
            || texture->pixels[1] > 140 || texture->pixels[2] < 115 || texture->pixels[2] > 140
            || texture->pixels[3] != 255)
            throw std::runtime_error("neutral JPEG texture decoding changed pixel data");

        {
            std::ofstream output(root / "textures/test.jpg", std::ios::binary | std::ios::trunc);
            output << "not a jpeg";
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.jpg")))
            throw std::runtime_error("malformed neutral JPEG unexpectedly decoded");

        std::filesystem::remove_all(root, error);
    }
#endif
}

int main()
{
    const VFS::Manager vfsManager;
    const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
    Resource::ResourceSystem resourceSystem(
        &vfsManager, 1.0, &encoder.getStatelessEncoder(), Resource::ResourceSystem::Backend::Neutral);

    if (resourceSystem.backend() != Resource::ResourceSystem::Backend::Neutral)
        throw std::runtime_error("neutral resource backend identity was not retained");

    if (resourceSystem.getSceneManager() != nullptr || resourceSystem.getKeyframeManager() != nullptr
        || resourceSystem.getImageManager() != nullptr || resourceSystem.getBgsmFileManager() != nullptr
        || resourceSystem.getAnimBlendRulesManager() != nullptr)
        throw std::runtime_error("neutral resource backend constructed OSG scene services");
    if (resourceSystem.getNifFileManager() == nullptr || resourceSystem.getNifMeshManager() == nullptr)
        throw std::runtime_error("neutral resource backend omitted shared resource services");
    if (resourceSystem.getNeutralTextureManager() == nullptr)
        throw std::runtime_error("neutral resource backend omitted the texture decoder");

    testNeutralBmpTexture();
#ifdef OPENMW_NEUTRAL_PNG
    testNeutralPngTexture();
#endif
#ifdef OPENMW_NEUTRAL_JPEG
    testNeutralJpegTexture();
#endif
}
