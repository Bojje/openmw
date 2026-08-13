#ifndef COMPONENTS_TERRAIN_RENDERSTORAGE_H
#define COMPONENTS_TERRAIN_RENDERSTORAGE_H

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
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

        // Drop backend-owned decoded terrain payloads at a world reset. The
        // reference adapter has no neutral cache and keeps the default no-op.
        virtual void clearCache() {}

        /// Prewarm backend-owned terrain payloads for half-open cell bounds:
        /// [minX, minY, maxX, maxY).
        /// The reference adapter keeps its existing asynchronous preloader;
        /// neutral backends may decode these cells synchronously or schedule
        /// them independently.
        virtual void preloadCells(std::span<const std::array<int, 4>> /*bounds*/, ESM::RefId /*worldspace*/) {}

        std::vector<Render::TerrainTile> getRenderTiles(int gridX, int gridY, ESM::RefId worldspace);

        // Assemble aligned square regions only from cells that are currently
        // active. This avoids treating holes in a paged exterior cell set as
        // loaded terrain merely because they fall inside its bounding box.
        std::vector<Render::TerrainRegion> getRenderRegionTiles(
            const std::set<std::pair<int, int>>& activeCells, ESM::RefId worldspace);

        std::optional<Render::TerrainTile> getRenderTile(
            int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace);
    };
}

#endif
