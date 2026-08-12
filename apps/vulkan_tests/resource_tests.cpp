#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
        {
            std::ofstream output(root / "textures/test.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
        }
        {
            std::ofstream output(root / "textures/test.dds", std::ios::binary);
            output.write(reinterpret_cast<const char*>(dds.data()), static_cast<std::streamsize>(dds.size()));
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
        const auto normal = resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.dds"));
        if (!normal || normal->width != 4 || normal->height != 4 || normal->pixels.size() != 4 * 4 * 4
            || normal->pixels[0] < 190 || normal->pixels[0] > 194 || normal->pixels[1] < 126
            || normal->pixels[1] > 130 || normal->pixels[2] < 235 || normal->pixels[2] > 240
            || normal->pixels[3] != 255 || normal->pixels[4] < 62 || normal->pixels[4] > 66)
            throw std::runtime_error("neutral BC5 texture decoding did not reconstruct a normal");
        {
            std::ofstream output(root / "textures/test.dds", std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(dds.data()), 128);
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.dds")))
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
