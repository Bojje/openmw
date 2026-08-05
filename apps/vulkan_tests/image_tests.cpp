#include <cstdlib>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

#include <components/render/imagecomparison.hpp>

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
}

int main()
{
    try
    {
        testExactMatch();
        testToleranceAndMetrics();
        testInvalidOrDifferentImages();
        std::cout << "Vulkan image comparison tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan image comparison test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
