#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

#include <components/render/imagecomparison.hpp>
#include <components/render/textureconversion.hpp>

namespace
{
    Render::TextureData makeImage(std::initializer_list<std::uint8_t> pixels)
    {
        Render::TextureData image;
        image.width = 2;
        image.height = 1;
        image.pixels = pixels;
        return image;
    }

    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    void testExactMatch()
    {
        const Render::TextureData image = makeImage({ 0, 32, 64, 255, 255, 128, 16, 0 });
        const Render::ImageComparison comparison = Render::compareImages(image, image);
        expect(comparison.matches(0), "identical images should match");
        expect(comparison.comparedPixels == 2, "exact comparison pixel count");
        expect(comparison.differingPixels == 0, "exact comparison difference count");
        expect(comparison.maxChannelError == 0, "exact comparison maximum error");
    }

    void testToleranceAndMetrics()
    {
        const Render::TextureData reference = makeImage({ 0, 32, 64, 255, 255, 128, 16, 0 });
        Render::TextureData candidate = reference;
        candidate.pixels[0] = 3;
        candidate.pixels[4] = 246;

        const Render::ImageComparison exact = Render::compareImages(reference, candidate);
        expect(!exact.matches(2), "an over-tolerance pixel should fail");
        expect(exact.differingPixels == 2, "difference count should be per pixel");
        expect(exact.maxChannelError == 9, "maximum channel error");
        expect(exact.totalChannelError == 12, "total channel error");

        const Render::ImageComparison tolerant = Render::compareImages(reference, candidate, 9);
        expect(tolerant.matches(9), "per-channel tolerance should be honored");
        expect(tolerant.meanChannelError() == 12.0 / 8.0, "mean channel error");
    }

    void testInvalidOrDifferentImages()
    {
        const Render::TextureData reference = makeImage({ 0, 0, 0, 255, 0, 0, 0, 255 });
        Render::TextureData invalid;
        expect(!Render::compareImages(reference, invalid).sameDimensions, "invalid image should not compare");

        Render::TextureData different = reference;
        different.width = 1;
        different.height = 2;
        expect(!Render::compareImages(reference, different).sameDimensions, "different dimensions should not compare");
    }

    void testPpmRoundTrip()
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "openmw-image-comparison.ppm";
        const Render::TextureData source = makeImage({ 0, 32, 64, 1, 255, 128, 16, 2 });
        expect(Render::writePpm(source, path), "PPM writer failed");
        const auto loaded = Render::readPpm(path);
        std::error_code error;
        std::filesystem::remove(path, error);
        expect(loaded.has_value(), "PPM reader failed");
        expect(loaded->width == source.width && loaded->height == source.height, "PPM dimensions changed");
        expect(loaded->pixels[0] == source.pixels[0] && loaded->pixels[1] == source.pixels[1]
                && loaded->pixels[2] == source.pixels[2] && loaded->pixels[3] == 255
                && loaded->pixels[4] == source.pixels[4] && loaded->pixels[6] == source.pixels[6]
                && loaded->pixels[7] == 255,
            "PPM RGBA conversion changed pixel data");
    }

    void testRgba8Conversion()
    {
        const Render::TextureData image = Render::makeRgba8Texture(2, 1, [](std::uint32_t x, std::uint32_t) {
            return std::array<float, 4>{ x == 0 ? -0.1f : 0.5f, 0.25f, 1.1f, 0.5f };
        });
        expect(image.valid(), "RGBA8 conversion should produce a valid image");
        expect(image.pixels == std::vector<std::uint8_t>({ 0, 64, 255, 128, 128, 64, 255, 128 }),
            "RGBA8 conversion should clamp and quantize channels consistently");

        const Render::TextureData invalid = Render::makeRgba8Texture(1, 1, [](std::uint32_t, std::uint32_t) {
            return std::array<float, 4>{ 0.f, std::numeric_limits<float>::quiet_NaN(), 0.f, 1.f };
        });
        expect(!invalid.valid(), "RGBA8 conversion should reject non-finite channels");
    }
}

int main()
{
    try
    {
        testExactMatch();
        testToleranceAndMetrics();
        testInvalidOrDifferentImages();
        testPpmRoundTrip();
        testRgba8Conversion();
        std::cout << "Vulkan image comparison tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan image comparison test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
