#include <cmath>
#include <cstdlib>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>

#include <components/vk/vkmath.hpp>

namespace
{
    constexpr float epsilon = 1e-5f;

    void expectNear(float actual, float expected, const std::string& label)
    {
        if (std::abs(actual - expected) > epsilon)
            throw std::runtime_error(label + ": expected " + std::to_string(expected) + ", got "
                + std::to_string(actual));
    }

    void expectMatrixEntry(const Vk::Mat4& matrix, int index, float expected, const std::string& label)
    {
        expectNear(matrix.data[index], expected, label + "[" + std::to_string(index) + "]");
    }

    Vk::Mat4 makeAffineTransform()
    {
        Vk::Mat4 transform = {};
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
        Vk::Mat4 input = {};
        for (int i = 0; i < 16; ++i)
            input.data[i] = static_cast<float>(i);

        const Vk::Mat4 transposed = Vk::transposeMat4(input);
        expectMatrixEntry(transposed, 1, 4.0f, "transpose");
        expectMatrixEntry(transposed, 4, 1.0f, "transpose");
        expectMatrixEntry(transposed, 6, 9.0f, "transpose");
        expectMatrixEntry(transposed, 9, 6.0f, "transpose");
    }

    void testAffineInverse()
    {
        const Vk::Mat4 transform = makeAffineTransform();
        const Vk::Mat4 inverse = Vk::invertAffine(transform);

        expectMatrixEntry(inverse, 0, 0.5f, "affine inverse");
        expectMatrixEntry(inverse, 5, 1.0f / 3.0f, "affine inverse");
        expectMatrixEntry(inverse, 10, 0.25f, "affine inverse");
        expectMatrixEntry(inverse, 12, -5.0f, "affine inverse");
        expectMatrixEntry(inverse, 13, -20.0f / 3.0f, "affine inverse");
        expectMatrixEntry(inverse, 14, -7.5f, "affine inverse");
        expectMatrixEntry(inverse, 15, 1.0f, "affine inverse");

        const Vk::Mat4 fullInverse = Vk::invertMat4(transform);
        for (int i : { 0, 5, 10, 12, 13, 14, 15 })
            expectMatrixEntry(fullInverse, i, inverse.data[i], "full inverse");
    }

    void testNormalMatrix()
    {
        const Vk::Mat4 normal = Vk::computeNormalMatrix(makeAffineTransform());
        expectMatrixEntry(normal, 0, 0.5f, "normal matrix");
        expectMatrixEntry(normal, 5, 1.0f / 3.0f, "normal matrix");
        expectMatrixEntry(normal, 10, 0.25f, "normal matrix");
    }

    void testSingularMatrices()
    {
        Vk::Mat4 singular = {};
        singular.data[15] = 1.0f;

        const Vk::Mat4 affineInverse = Vk::invertAffine(singular);
        const Vk::Mat4 fullInverse = Vk::invertMat4(singular);
        for (int i = 0; i < 16; ++i)
        {
            expectMatrixEntry(affineInverse, i, 0.0f, "singular affine inverse");
            expectMatrixEntry(fullInverse, i, 0.0f, "singular full inverse");
        }
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
        std::cout << "Vulkan math tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan math test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
