#include "meshconversion.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace Render
{
    MeshData makeMeshData(std::span<const MeshVertexSource> source)
    {
        MeshData result;
        result.vertices.resize(source.size());

        for (std::size_t i = 0; i < source.size(); ++i)
        {
            const MeshVertexSource& sourceVertex = source[i];
            MeshVertex& vertex = result.vertices[i];
            for (std::size_t axis = 0; axis < sourceVertex.position.size(); ++axis)
                vertex.position[axis] = sourceVertex.position[axis];
            for (std::size_t axis = 0; axis < sourceVertex.normal.size(); ++axis)
                vertex.normal[axis] = sourceVertex.hasNormal ? sourceVertex.normal[axis] : (axis == 2 ? 1.f : 0.f);
            for (std::size_t axis = 0; axis < sourceVertex.texcoord.size(); ++axis)
            {
                vertex.texcoord[axis] = sourceVertex.hasTexcoord ? sourceVertex.texcoord[axis] : 0.f;
                vertex.blendTexcoord[axis] = vertex.texcoord[axis];
            }
            for (std::size_t axis = 0; axis < sourceVertex.color.size(); ++axis)
                vertex.color[axis] = sourceVertex.hasColor ? sourceVertex.color[axis] : 1.f;

            vertex.material[0] = 1.f;
            vertex.material[1] = 0.f;
            vertex.material[2] = 1.f;
            vertex.material[3] = 0.f;
            vertex.tangent[0] = 1.f;
            vertex.tangent[1] = 0.f;
            vertex.tangent[2] = 0.f;
            vertex.tangent[3] = 1.f;
        }

        return result;
    }

    void appendMeshIndex(MeshData& mesh, std::uint32_t index)
    {
        if (index >= mesh.vertices.size())
            throw std::runtime_error("mesh triangle index is outside the vertex data");
        mesh.indices.push_back(index);
    }

    void computeMeshTangents(MeshData& mesh)
    {
        std::vector<std::array<float, 3>> tangents(mesh.vertices.size());
        std::vector<std::array<float, 3>> bitangents(mesh.vertices.size());

        const auto add = [](std::array<float, 3>& target, const std::array<float, 3>& value) {
            for (std::size_t axis = 0; axis < 3; ++axis)
                target[axis] += value[axis];
        };
        const auto edge = [&mesh](std::uint32_t index, std::uint32_t origin) {
            return std::array<float, 3>{ mesh.vertices[index].position[0] - mesh.vertices[origin].position[0],
                mesh.vertices[index].position[1] - mesh.vertices[origin].position[1],
                mesh.vertices[index].position[2] - mesh.vertices[origin].position[2] };
        };

        for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
        {
            const std::uint32_t i0 = mesh.indices[triangle + 0];
            const std::uint32_t i1 = mesh.indices[triangle + 1];
            const std::uint32_t i2 = mesh.indices[triangle + 2];
            const float du1 = mesh.vertices[i1].texcoord[0] - mesh.vertices[i0].texcoord[0];
            const float dv1 = mesh.vertices[i1].texcoord[1] - mesh.vertices[i0].texcoord[1];
            const float du2 = mesh.vertices[i2].texcoord[0] - mesh.vertices[i0].texcoord[0];
            const float dv2 = mesh.vertices[i2].texcoord[1] - mesh.vertices[i0].texcoord[1];
            const float determinant = du1 * dv2 - du2 * dv1;
            if (std::abs(determinant) < 1e-6f)
                continue;

            const std::array<float, 3> edge1 = edge(i1, i0);
            const std::array<float, 3> edge2 = edge(i2, i0);
            const float inverseDeterminant = 1.f / determinant;
            const std::array<float, 3> tangent = {
                (edge1[0] * dv2 - edge2[0] * dv1) * inverseDeterminant,
                (edge1[1] * dv2 - edge2[1] * dv1) * inverseDeterminant,
                (edge1[2] * dv2 - edge2[2] * dv1) * inverseDeterminant,
            };
            const std::array<float, 3> bitangent = {
                (edge2[0] * du1 - edge1[0] * du2) * inverseDeterminant,
                (edge2[1] * du1 - edge1[1] * du2) * inverseDeterminant,
                (edge2[2] * du1 - edge1[2] * du2) * inverseDeterminant,
            };
            add(tangents[i0], tangent);
            add(tangents[i1], tangent);
            add(tangents[i2], tangent);
            add(bitangents[i0], bitangent);
            add(bitangents[i1], bitangent);
            add(bitangents[i2], bitangent);
        }

        for (std::size_t vertexIndex = 0; vertexIndex < mesh.vertices.size(); ++vertexIndex)
        {
            MeshVertex& vertex = mesh.vertices[vertexIndex];
            const std::array<float, 3> normal = { vertex.normal[0], vertex.normal[1], vertex.normal[2] };
            std::array<float, 3> tangent = tangents[vertexIndex];
            const float normalLength = std::sqrt(
                normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            const std::array<float, 3> unitNormal = normalLength > 1e-6f
                ? std::array<float, 3>{ normal[0] / normalLength, normal[1] / normalLength,
                      normal[2] / normalLength }
                : std::array<float, 3>{ 0.f, 0.f, 1.f };
            const float projection = tangent[0] * unitNormal[0] + tangent[1] * unitNormal[1]
                + tangent[2] * unitNormal[2];
            for (std::size_t axis = 0; axis < 3; ++axis)
                tangent[axis] -= unitNormal[axis] * projection;

            float tangentLength = std::sqrt(
                tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
            if (tangentLength <= 1e-6f)
            {
                const std::array<float, 3> axis = std::abs(unitNormal[2]) < 0.9f
                    ? std::array<float, 3>{ 0.f, 0.f, 1.f }
                    : std::array<float, 3>{ 0.f, 1.f, 0.f };
                tangent = { axis[1] * unitNormal[2] - axis[2] * unitNormal[1],
                    axis[2] * unitNormal[0] - axis[0] * unitNormal[2],
                    axis[0] * unitNormal[1] - axis[1] * unitNormal[0] };
                tangentLength = std::sqrt(
                    tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
            }
            for (float& component : tangent)
                component /= tangentLength;

            const std::array<float, 3> bitangent = {
                unitNormal[1] * tangent[2] - unitNormal[2] * tangent[1],
                unitNormal[2] * tangent[0] - unitNormal[0] * tangent[2],
                unitNormal[0] * tangent[1] - unitNormal[1] * tangent[0],
            };
            const float handedness = bitangent[0] * bitangents[vertexIndex][0]
                    + bitangent[1] * bitangents[vertexIndex][1]
                    + bitangent[2] * bitangents[vertexIndex][2]
                < 0.f
                ? -1.f
                : 1.f;
            vertex.tangent[0] = tangent[0];
            vertex.tangent[1] = tangent[1];
            vertex.tangent[2] = tangent[2];
            vertex.tangent[3] = handedness;
        }
    }
}
