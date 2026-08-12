#include <cmath>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

#include <components/render/math.hpp>

namespace
{
    constexpr float epsilon = 1e-5f;

    static_assert(sizeof(Render::Mat4) == 64);
    static_assert(sizeof(Render::SceneData) == 352);

    void expectNear(float actual, float expected, const std::string& label)
    {
        if (std::abs(actual - expected) > epsilon)
            throw std::runtime_error(label + ": expected " + std::to_string(expected) + ", got "
                + std::to_string(actual));
    }

    void expectMatrixEntry(const Render::Mat4& matrix, int index, float expected, const std::string& label)
    {
        expectNear(matrix.data[index], expected, label + "[" + std::to_string(index) + "]");
    }

    Render::Mat4 makeAffineTransform()
    {
        Render::Mat4 transform = {};
        transform.data[0] = 2.0f;
        transform.data[5] = 3.0f;
        transform.data[10] = 4.0f;
        transform.data[12] = 10.0f;
        transform.data[13] = 20.0f;
        transform.data[14] = 30.0f;
        transform.data[15] = 1.0f;
        return transform;
    }

    void testTranspose()
    {
        Render::Mat4 input = {};
        for (int i = 0; i < 16; ++i)
            input.data[i] = static_cast<float>(i);

        const Render::Mat4 transposed = Render::transposeMat4(input);
        expectMatrixEntry(transposed, 1, 4.0f, "transpose");
        expectMatrixEntry(transposed, 4, 1.0f, "transpose");
        expectMatrixEntry(transposed, 6, 9.0f, "transpose");
        expectMatrixEntry(transposed, 9, 6.0f, "transpose");
    }

    void testAffineInverse()
    {
        const Render::Mat4 transform = makeAffineTransform();
        const Render::Mat4 inverse = Render::invertAffine(transform);

        expectMatrixEntry(inverse, 0, 0.5f, "affine inverse");
        expectMatrixEntry(inverse, 5, 1.0f / 3.0f, "affine inverse");
        expectMatrixEntry(inverse, 10, 0.25f, "affine inverse");
        expectMatrixEntry(inverse, 12, -5.0f, "affine inverse");
        expectMatrixEntry(inverse, 13, -20.0f / 3.0f, "affine inverse");
        expectMatrixEntry(inverse, 14, -7.5f, "affine inverse");
        expectMatrixEntry(inverse, 15, 1.0f, "affine inverse");

        const Render::Mat4 fullInverse = Render::invertMat4(transform);
        for (int i : { 0, 5, 10, 12, 13, 14, 15 })
            expectMatrixEntry(fullInverse, i, inverse.data[i], "full inverse");
    }

    void testNormalMatrix()
    {
        const Render::Mat4 normal = Render::computeNormalMatrix(makeAffineTransform());
        expectMatrixEntry(normal, 0, 0.5f, "normal matrix");
        expectMatrixEntry(normal, 5, 1.0f / 3.0f, "normal matrix");
        expectMatrixEntry(normal, 10, 0.25f, "normal matrix");
    }

    void testSingularMatrices()
    {
        Render::Mat4 singular = {};
        singular.data[15] = 1.0f;

        const Render::Mat4 affineInverse = Render::invertAffine(singular);
        const Render::Mat4 fullInverse = Render::invertMat4(singular);
        for (int i = 0; i < 16; ++i)
        {
            expectMatrixEntry(affineInverse, i, 0.0f, "singular affine inverse");
            expectMatrixEntry(fullInverse, i, 0.0f, "singular full inverse");
        }
    }

    void testSceneDataValidation()
    {
        Render::SceneData scene;
        if (!scene.valid() || std::abs(scene.skyColor.x - 0.6f) > epsilon)
            throw std::runtime_error("renderer-neutral scene sky state did not initialize");

        scene.skyColor.x = std::numeric_limits<float>::quiet_NaN();
        if (scene.valid())
            throw std::runtime_error("renderer-neutral scene accepted a non-finite sky color");
    }
}

int main()
{
    try
    {
        testTranspose();
        testAffineInverse();
        testNormalMatrix();
        testSingularMatrices();
        testSceneDataValidation();
        std::cout << "Vulkan math tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan math test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
