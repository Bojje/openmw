#include "meshconverter.hpp"

#include <stdexcept>

#include "data.hpp"

namespace Nif
{
    Render::MeshData convertMesh(const NiTriShapeData& source)
    {
        if (source.mTriangles.size() % 3 != 0)
            throw std::runtime_error("NIF triangle index data is not a multiple of three");

        Render::MeshData result;
        result.vertices.resize(source.mVertices.size());
        result.indices.reserve(source.mTriangles.size());

        const bool hasNormals = source.mNormals.size() == source.mVertices.size();
        const bool hasColors = source.mColors.size() == source.mVertices.size();
        const bool hasTexcoords = !source.mUVList.empty()
            && source.mUVList.front().size() == source.mVertices.size();

        for (std::size_t i = 0; i < source.mVertices.size(); ++i)
        {
            const auto& position = source.mVertices[i];
            auto& vertex = result.vertices[i];
            vertex.position[0] = position.x();
            vertex.position[1] = position.y();
            vertex.position[2] = position.z();

            if (hasNormals)
            {
                const auto& normal = source.mNormals[i];
                vertex.normal[0] = normal.x();
                vertex.normal[1] = normal.y();
                vertex.normal[2] = normal.z();
            }
            else
            {
                vertex.normal[0] = 0.0f;
                vertex.normal[1] = 0.0f;
                vertex.normal[2] = 1.0f;
            }

            if (hasTexcoords)
            {
                const auto& texcoord = source.mUVList.front()[i];
                vertex.texcoord[0] = texcoord.x();
                vertex.texcoord[1] = texcoord.y();
            }
            else
            {
                vertex.texcoord[0] = 0.0f;
                vertex.texcoord[1] = 0.0f;
            }

            if (hasColors)
            {
                const auto& color = source.mColors[i];
                vertex.color[0] = color.x();
                vertex.color[1] = color.y();
                vertex.color[2] = color.z();
                vertex.color[3] = color.w();
            }
            else
            {
                vertex.color[0] = 1.0f;
                vertex.color[1] = 1.0f;
                vertex.color[2] = 1.0f;
                vertex.color[3] = 1.0f;
            }
        }

        for (unsigned short index : source.mTriangles)
        {
            if (index >= result.vertices.size())
                throw std::runtime_error("NIF triangle index is outside the vertex data");
            result.indices.push_back(index);
        }

        return result;
    }
}
