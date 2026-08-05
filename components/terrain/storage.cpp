#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

#include <osg/Image>

namespace Terrain
{
    namespace
    {
        Render::TextureData convertBlendmap(const osg::Image& image)
        {
            Render::TextureData result;
            result.width = static_cast<std::uint32_t>(image.s());
            result.height = static_cast<std::uint32_t>(image.t());
            if (result.width == 0 || result.height == 0)
            {
                result.width = 0;
                result.height = 0;
                return result;
            }

            result.pixels.resize(static_cast<std::size_t>(result.width) * result.height * 4);
            for (std::uint32_t y = 0; y < result.height; ++y)
            {
                for (std::uint32_t x = 0; x < result.width; ++x)
                {
                    const osg::Vec4 color = image.getColor(x, y);
                    const std::size_t offset = (static_cast<std::size_t>(y) * result.width + x) * 4;
                    const auto toByte = [](float value) {
                        return static_cast<std::uint8_t>(std::clamp(value, 0.f, 1.f) * 255.f + 0.5f);
                    };
                    result.pixels[offset + 0] = toByte(color.r());
                    result.pixels[offset + 1] = toByte(color.g());
                    result.pixels[offset + 2] = toByte(color.b());
                    result.pixels[offset + 3] = toByte(color.a());
                }
            }
            return result;
        }
    }

    std::optional<Render::TerrainTile> Storage::getRenderTile(
        int lodLevel, float size, const osg::Vec2f& center, ESM::RefId worldspace)
    {
        if (lodLevel < 0 || size <= 0.f)
            return std::nullopt;

        osg::ref_ptr<osg::Vec3Array> positions = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> normals = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4ubArray> colors = new osg::Vec4ubArray;
        fillVertexBuffers(lodLevel, size, center, worldspace, *positions, *normals, *colors);
        if (positions->size() != normals->size() || positions->size() != colors->size())
            return std::nullopt;

        Render::TerrainTile tile;
        tile.lod = lodLevel;
        tile.size = size;
        tile.center = { center.x(), center.y() };
        tile.verticesPerSide = static_cast<std::uint32_t>(
            std::sqrt(static_cast<double>(positions->size())));
        while (static_cast<std::size_t>(tile.verticesPerSide + 1) * (tile.verticesPerSide + 1)
               <= positions->size())
            ++tile.verticesPerSide;
        while (static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide > positions->size())
            --tile.verticesPerSide;
        if (static_cast<std::size_t>(tile.verticesPerSide) * tile.verticesPerSide != positions->size())
            return std::nullopt;
        tile.vertices.resize(positions->size());
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
        for (std::size_t i = 0; i < positions->size(); ++i)
        {
            tile.vertices[i].position = { (*positions)[i].x(), (*positions)[i].y(), (*positions)[i].z() };
            tile.vertices[i].normal = { (*normals)[i].x(), (*normals)[i].y(), (*normals)[i].z() };
            tile.vertices[i].color = { (*colors)[i].r(), (*colors)[i].g(), (*colors)[i].b(), (*colors)[i].a() };
        }

        ImageVector blendmaps;
        std::vector<LayerInfo> layerList;
        getBlendmaps(size, center, blendmaps, layerList, worldspace);
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
                if (blendmaps[i] == nullptr)
                    return std::nullopt;
                layer.blendmap = convertBlendmap(*blendmaps[i]);
                if (!layer.blendmap.valid())
                    return std::nullopt;
            }
        }
        return tile;
    }
}
