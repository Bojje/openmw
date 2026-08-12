#include <array>
#ifdef OPENMW_NEUTRAL_JPEG
#include <csetjmp>
#include <cstdio>
#include <jpeglib.h>
#endif
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include <components/files/collections.hpp>
#include <components/resource/neutraltexturemanager.hpp>
#include <components/resource/resourcesystem.hpp>
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
        {
            std::ofstream output(root / "textures/test.bmp", std::ios::binary);
            output.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
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

        // 1x1 RGBA PNG containing an opaque red pixel.
        constexpr std::array<unsigned char, 70> png = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
            0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
            0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x08, 0xd7, 0x63, 0xf8, 0xcf, 0xc0, 0xf0,
            0x1f, 0x00, 0x05, 0x00, 0x01, 0xff, 0x89, 0x99, 0x3d, 0x1d, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
            0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };
        {
            std::ofstream output(root / "textures/test.png", std::ios::binary);
            output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
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
            output.write(reinterpret_cast<const char*>(png.data()), 32);
        }
        resources.getNeutralTextureManager()->clearCache();
        if (resources.getNeutralTextureManager()->get(VFS::Path::Normalized("textures/test.png")))
            throw std::runtime_error("malformed neutral PNG unexpectedly decoded");

        std::filesystem::remove_all(root, error);
    }
#endif

#ifdef OPENMW_NEUTRAL_JPEG
    struct JpegTestError
    {
        jpeg_error_mgr manager;
        jmp_buf jump;
    };

    void jpegTestErrorExit(j_common_ptr cinfo)
    {
        auto* const error = reinterpret_cast<JpegTestError*>(cinfo->err);
        longjmp(error->jump, 1);
    }

    void writeTestJpeg(const std::filesystem::path& path)
    {
        FILE* const file = std::fopen(path.string().c_str(), "wb");
        if (!file)
            throw std::runtime_error("could not open neutral JPEG test file");

        jpeg_compress_struct jpeg{};
        JpegTestError error{};
        jpeg.err = jpeg_std_error(&error.manager);
        error.manager.error_exit = jpegTestErrorExit;
        bool created = false;
        if (setjmp(error.jump) != 0)
        {
            if (created)
                jpeg_destroy_compress(&jpeg);
            std::fclose(file);
            throw std::runtime_error("could not encode neutral JPEG test file");
        }

        jpeg_create_compress(&jpeg);
        created = true;
        jpeg_stdio_dest(&jpeg, file);
        jpeg.image_width = 1;
        jpeg.image_height = 1;
        jpeg.input_components = 1;
        jpeg.in_color_space = JCS_GRAYSCALE;
        jpeg_set_defaults(&jpeg);
        jpeg_set_quality(&jpeg, 100, TRUE);
        jpeg_start_compress(&jpeg, TRUE);
        JSAMPLE pixel = 127;
        JSAMPROW row = &pixel;
        jpeg_write_scanlines(&jpeg, &row, 1);
        jpeg_finish_compress(&jpeg);
        jpeg_destroy_compress(&jpeg);
        std::fclose(file);
    }

    void testNeutralJpegTexture()
    {
        const std::filesystem::path root = std::filesystem::temp_directory_path() / "openmw-neutral-jpeg-test";
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "textures", error);
        if (error)
            throw std::runtime_error("could not create neutral JPEG test directory");
        writeTestJpeg(root / "textures/test.jpg");

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
