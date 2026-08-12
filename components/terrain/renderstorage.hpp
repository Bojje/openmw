#ifndef COMPONENTS_TERRAIN_RENDERSTORAGE_H
#define COMPONENTS_TERRAIN_RENDERSTORAGE_H

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <components/esm/refid.hpp>
#include <components/render/scene.hpp>
#include <components/render/terrain.hpp>

#include "defs.hpp"

namespace Terrain
{
    /// Renderer-neutral terrain data source. It deliberately has no OSG
    /// arrays, images, or scene-node types; the legacy terrain adapter derives
    /// from it for the reference renderer.
    class RenderStorage
    {
    public:
        virtual ~RenderStorage() = default;

        virtual void getBounds(float& minX, float& maxX, float& minY, float& maxY, ESM::RefId worldspace) = 0;

        virtual void fillRenderVertexBuffers(int lodLevel, float size, const std::array<float, 2>& center,
            ESM::RefId worldspace, std::vector<Render::TerrainVertex>& vertices)
            = 0;

        virtual void getRenderBlendmaps(float chunkSize, const std::array<float, 2>& chunkCenter,
            std::vector<Render::TextureData>& blendmaps, std::vector<LayerInfo>& layerList,
            ESM::RefId worldspace)
            = 0;

        virtual float getCellWorldSize(ESM::RefId worldspace) = 0;
        virtual int getCellVertices(ESM::RefId worldspace) = 0;
        virtual int getTextureTileCount(float chunkSize, ESM::RefId worldspace) = 0;

        /// Query terrain height without exposing the legacy terrain-world API.
        /// Adapters with no terrain data may return their documented default.
        virtual float getHeightAt(const Render::Vec3& worldPos, ESM::RefId worldspace)
        {
            return 0.f;
        }

        /// Optional neutral collision samples for one exterior cell.
        virtual std::optional<Render::TerrainHeightField> getHeightField(
            int /*gridX*/, int /*gridY*/, ESM::RefId /*worldspace*/)
        {
            return std::nullopt;
        }

        std::vector<Render::TerrainTile> getRenderTiles(int gridX, int gridY, ESM::RefId worldspace);

        // Assemble aligned square regions covering an exterior cell rectangle.
        // Invalid regions are retained in the result so callers can reject a
        // partial quadtree set and keep using their per-cell fallback.
        std::vector<Render::TerrainRegion> getRenderRegionTiles(
            int minCellX, int maxCellX, int minCellY, int maxCellY, ESM::RefId worldspace);

        std::optional<Render::TerrainTile> getRenderTile(
            int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace);
    };
}

#endif
