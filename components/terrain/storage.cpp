#include "storage.hpp"

#include <cstring>

#include <osg/Image>

#include <components/render/textureconversion.hpp>

namespace Terrain
{
    void Storage::fillVertexBuffers(int lodLevel, float size, const osg::Vec2f& center, ESM::RefId worldspace,
        osg::Vec3Array& positions, osg::Vec3Array& normals, osg::Vec4ubArray& colours)
    {
        std::vector<Render::TerrainVertex> vertices;
        fillRenderVertexBuffers(lodLevel, size, { center.x(), center.y() }, worldspace, vertices);

        positions.resize(vertices.size());
        normals.resize(vertices.size());
        colours.resize(vertices.size());
        for (std::size_t i = 0; i < vertices.size(); ++i)
        {
            const Render::TerrainVertex& vertex = vertices[i];
            positions[i].set(vertex.position[0], vertex.position[1], vertex.position[2]);
            normals[i].set(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
            colours[i].set(vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]);
        }
    }

    void Storage::getBlendmaps(float chunkSize, const osg::Vec2f& chunkCenter, ImageVector& blendmaps,
        std::vector<LayerInfo>& layerList, ESM::RefId worldspace)
    {
        std::vector<Render::TextureData> neutralBlendmaps;
        getRenderBlendmaps(chunkSize, { chunkCenter.x(), chunkCenter.y() }, neutralBlendmaps, layerList, worldspace);

        blendmaps.clear();
        blendmaps.reserve(neutralBlendmaps.size());
        for (const Render::TextureData& texture : neutralBlendmaps)
        {
            if (!texture.valid())
            {
                blendmaps.clear();
                return;
            }
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(static_cast<int>(texture.width), static_cast<int>(texture.height), 1, GL_RGBA,
                GL_UNSIGNED_BYTE);
            std::memcpy(image->data(), texture.pixels.data(), texture.pixels.size());
            blendmaps.push_back(std::move(image));
        }
    }

}
