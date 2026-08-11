#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

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

    std::optional<Render::TerrainTile> Storage::getRenderTile(
        int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace)
    {
        if (lodLevel < 0 || size <= 0.f)
            return std::nullopt;

        std::vector<Render::TerrainVertex> vertices;
        fillRenderVertexBuffers(lodLevel, size, center, worldspace, vertices);

        Render::TerrainTile tile;
        tile.lod = lodLevel;
        tile.size = size;
        tile.center = center;
        tile.cellWorldSize = getCellWorldSize(worldspace);
        if (tile.cellWorldSize <= 0.f)
            return std::nullopt;
        tile.blendmapScale = static_cast<float>(getTextureTileCount(size, worldspace));
        if (tile.blendmapScale <= 0.f)
            return std::nullopt;
        tile.verticesPerSide = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(vertices.size())));
        while (static_cast<std::size_t>(tile.verticesPerSide + 1) * (tile.verticesPerSide + 1) <= vertices.size())
            ++tile.verticesPerSide;
        while (static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide > vertices.size())
            --tile.verticesPerSide;
        if (tile.verticesPerSide < 2
            || static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide != vertices.size())
            return std::nullopt;
        tile.vertices = std::move(vertices);
        tile.indices.reserve(static_cast<std::size_t>(tile.verticesPerSide - 1)
            * (tile.verticesPerSide - 1) * 6);
        for (std::uint32_t y = 0; y + 1 < tile.verticesPerSide; ++y)
        {
            for (std::uint32_t x = 0; x + 1 < tile.verticesPerSide; ++x)
            {
                const std::uint32_t topLeft = y * tile.verticesPerSide + x;
                const std::uint32_t topRight = topLeft + 1;
                const std::uint32_t bottomLeft = topLeft + tile.verticesPerSide;
                const std::uint32_t bottomRight = bottomLeft + 1;
                tile.indices.insert(tile.indices.end(), { topLeft, bottomLeft, topRight,
                    topRight, bottomLeft, bottomRight });
            }
        }
        std::vector<Render::TextureData> blendmaps;
        std::vector<LayerInfo> layerList;
        getRenderBlendmaps(size, center, blendmaps, layerList, worldspace);
        // A single opaque layer intentionally has no blendmap in the legacy
        // storage contract. Preserve that layer while leaving its neutral
        // blendmap invalid; the Vulkan consumer can treat it as fully opaque.
        if (!blendmaps.empty() && blendmaps.size() != layerList.size())
            return std::nullopt;

        tile.layers.reserve(layerList.size());
        for (std::size_t i = 0; i < layerList.size(); ++i)
        {
            Render::TerrainLayer& layer = tile.layers.emplace_back();
            layer.diffuseTexture = layerList[i].mDiffuseMap.value();
            layer.normalTexture = layerList[i].mNormalMap.value();
            layer.parallax = layerList[i].mParallax;
            layer.specular = layerList[i].mSpecular;
            if (i < blendmaps.size())
            {
                layer.blendmap = std::move(blendmaps[i]);
                if (!layer.blendmap.valid())
                    return std::nullopt;
            }
        }
        return tile;
    }
}
