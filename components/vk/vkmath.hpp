#ifndef OPENMW_COMPONENTS_VK_VKMATH_H
#define OPENMW_COMPONENTS_VK_VKMATH_H

#include <cmath>
#include <cstring>

namespace Vk
{
    struct Mat4
    {
        float data[16];
    };

    struct Vec3
    {
        float x, y, z;
    };

    struct Vec4
    {
        float x, y, z, w;
    };

    // The two helpers below take raw float[16] rather than Mat4. Transforms that flow through the
    // NIF converter and the rendering manager are stored as plain float[16] members inside their
    // own structs (NifVk::VulkanMesh::transform, CellMeshes::Instance::transform), so a raw-array
    // interface lets those call sites pass their members directly instead of copying in and out of
    // a Mat4. The layout is identical to Mat4::data, so `Vk::multiplyMat4(a.data, b.data, r.data)`
    // works unchanged for Mat4 callers.

    // Writes the 4x4 identity matrix.
    inline void identityMat4(float out[16])
    {
        std::memset(out, 0, 16 * sizeof(float));
        out[0] = 1.f;
        out[5] = 1.f;
        out[10] = 1.f;
        out[15] = 1.f;
    }

    // Column-major 4x4 matrix multiply: out = a * b
    // Column-major: element (row, col) is at index [col * 4 + row]
    // Safe to alias: `out` may be the same array as `a` or `b`.
    inline void multiplyMat4(const float a[16], const float b[16], float out[16])
    {
        float tmp[16];
        for (int col = 0; col < 4; ++col)
        {
            for (int row = 0; row < 4; ++row)
            {
                float sum = 0.f;
                for (int k = 0; k < 4; ++k)
                    sum += a[k * 4 + row] * b[col * 4 + k];
                tmp[col * 4 + row] = sum;
            }
        }
        std::memcpy(out, tmp, 16 * sizeof(float));
    }

    // 4x4 matrix transpose. Data is column-major: data[col*4 + row].
    inline Mat4 transposeMat4(const Mat4& m)
    {
        Mat4 result;
        for (int col = 0; col < 4; col++)
            for (int row = 0; row < 4; row++)
                result.data[col * 4 + row] = m.data[row * 4 + col];
        return result;
    }

    // Full 4x4 matrix inverse using Laplace expansion with 2x2 minors.
    // Column-major layout: element at row r, col c is data[c*4 + r].
    // Returns zero matrix if singular.
    inline Mat4 invertMat4(const Mat4& m)
    {
        Mat4 inv;
        const float* a = m.data;
        float* o = inv.data;

        float s0 = a[0] * a[5] - a[4] * a[1];
        float s1 = a[0] * a[6] - a[4] * a[2];
        float s2 = a[0] * a[7] - a[4] * a[3];
        float s3 = a[1] * a[6] - a[5] * a[2];
        float s4 = a[1] * a[7] - a[5] * a[3];
        float s5 = a[2] * a[7] - a[6] * a[3];

        float c5 = a[10] * a[15] - a[14] * a[11];
        float c4 = a[9] * a[15] - a[13] * a[11];
        float c3 = a[9] * a[14] - a[13] * a[10];
        float c2 = a[8] * a[15] - a[12] * a[11];
        float c1 = a[8] * a[14] - a[12] * a[10];
        float c0 = a[8] * a[13] - a[12] * a[9];

        float det = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
        if (std::abs(det) < 1e-12f)
        {
            std::memset(o, 0, sizeof(float) * 16);
            return inv;
        }

        float invDet = 1.0f / det;

        o[0] = (a[5] * c5 - a[6] * c4 + a[7] * c3) * invDet;
        o[4] = (-a[4] * c5 + a[6] * c2 - a[7] * c1) * invDet;
        o[8] = (a[4] * c4 - a[5] * c2 + a[7] * c0) * invDet;
        o[12] = (-a[4] * c3 + a[5] * c1 - a[6] * c0) * invDet;

        o[1] = (-a[1] * c5 + a[2] * c4 - a[3] * c3) * invDet;
        o[5] = (a[0] * c5 - a[2] * c2 + a[3] * c1) * invDet;
        o[9] = (-a[0] * c4 + a[1] * c2 - a[3] * c0) * invDet;
        o[13] = (a[0] * c3 - a[1] * c1 + a[2] * c0) * invDet;

        o[2] = (a[13] * s5 - a[14] * s4 + a[15] * s3) * invDet;
        o[6] = (-a[12] * s5 + a[14] * s2 - a[15] * s1) * invDet;
        o[10] = (a[12] * s4 - a[13] * s2 + a[15] * s0) * invDet;
        o[14] = (-a[12] * s3 + a[13] * s1 - a[14] * s0) * invDet;

        o[3] = (-a[9] * s5 + a[10] * s4 - a[11] * s3) * invDet;
        o[7] = (a[8] * s5 - a[10] * s2 + a[11] * s1) * invDet;
        o[11] = (-a[8] * s4 + a[9] * s2 - a[11] * s0) * invDet;
        o[15] = (a[8] * s3 - a[9] * s1 + a[10] * s0) * invDet;

        return inv;
    }

    // Fast inverse for affine matrices (bottom row = [0,0,0,1]).
    // Decomposes as: inv(R*S | t) = (inv(R*S) | -inv(R*S)*t).
    // Only inverts the upper-left 3x3 block via cofactor/determinant,
    // then transforms the translation component. Returns zero matrix if singular.
    inline Mat4 invertAffine(const Mat4& m)
    {
        const float* a = m.data;

        // Upper-left 3x3 block elements (column-major)
        float a00 = a[0], a01 = a[4], a02 = a[8];
        float a10 = a[1], a11 = a[5], a12 = a[9];
        float a20 = a[2], a21 = a[6], a22 = a[10];

        float det = a00 * (a11 * a22 - a12 * a21)
                  - a01 * (a10 * a22 - a12 * a20)
                  + a02 * (a10 * a21 - a11 * a20);

        if (std::abs(det) < 1e-12f)
        {
            Mat4 zero;
            std::memset(zero.data, 0, sizeof(zero.data));
            return zero;
        }

        float invDet = 1.0f / det;

        Mat4 r;
        // Inverse of 3x3 block
        r.data[0]  = (a11 * a22 - a12 * a21) * invDet;
        r.data[1]  = (a12 * a20 - a10 * a22) * invDet;
        r.data[2]  = (a10 * a21 - a11 * a20) * invDet;
        r.data[4]  = (a02 * a21 - a01 * a22) * invDet;
        r.data[5]  = (a00 * a22 - a02 * a20) * invDet;
        r.data[6]  = (a01 * a20 - a00 * a21) * invDet;
        r.data[8]  = (a01 * a12 - a02 * a11) * invDet;
        r.data[9]  = (a02 * a10 - a00 * a12) * invDet;
        r.data[10] = (a00 * a11 - a01 * a10) * invDet;

        // Translation: -inv(R*S) * t
        float tx = a[12], ty = a[13], tz = a[14];
        r.data[12] = -(r.data[0] * tx + r.data[4] * ty + r.data[8] * tz);
        r.data[13] = -(r.data[1] * tx + r.data[5] * ty + r.data[9] * tz);
        r.data[14] = -(r.data[2] * tx + r.data[6] * ty + r.data[10] * tz);

        // Bottom row
        r.data[3] = 0.f;
        r.data[7] = 0.f;
        r.data[11] = 0.f;
        r.data[15] = 1.f;

        return r;
    }

    // Converts an OpenGL-convention projection matrix (as produced by OSG) to Vulkan conventions.
    // OpenGL clip space has Y up and depth in [-1, 1]; Vulkan has Y down and depth in [0, 1].
    // Equivalent to C * glProjection with
    //     C = [1  0    0    0]
    //         [0 -1    0    0]
    //         [0  0  0.5  0.5]
    //         [0  0    0    1]
    // Column-major layout: element at row r, col c is data[c*4 + r].
    inline Mat4 glToVulkanProjection(const Mat4& glProjection)
    {
        Mat4 r;
        const float* p = glProjection.data;
        for (int col = 0; col < 4; col++)
        {
            r.data[col * 4 + 0] = p[col * 4 + 0];
            r.data[col * 4 + 1] = -p[col * 4 + 1];
            r.data[col * 4 + 2] = 0.5f * (p[col * 4 + 2] + p[col * 4 + 3]);
            r.data[col * 4 + 3] = p[col * 4 + 3];
        }
        return r;
    }

    // Compute the normal matrix for a model transform: transpose(inverse(model)).
    // Uses the affine fast path since model transforms are always affine.
    inline Mat4 computeNormalMatrix(const Mat4& model)
    {
        return transposeMat4(invertAffine(model));
    }
}

#endif
