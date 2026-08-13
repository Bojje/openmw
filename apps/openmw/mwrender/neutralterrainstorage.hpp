#ifndef OPENMW_MWRENDER_NEUTRAL_TERRAIN_STORAGE_H
#define OPENMW_MWRENDER_NEUTRAL_TERRAIN_STORAGE_H

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>

#include <components/terrain/renderstorage.hpp>
#include <components/vfs/pathutil.hpp>

namespace ESM
{
    class LandData;
}

namespace MWWorld
{
    class ESMStore;
}

namespace VFS
{
    class Manager;
}

namespace MWRender
{
    /// Terrain data source for the neutral renderer.
    ///
    /// This class reads ESM records directly and owns no OSG terrain objects.
    /// It supports both TES3 landscape records and ESM4 world/layer mapping.
    /// Decoded cells and generated neutral tiles are cached independently so
    /// scene refreshes do not repeatedly rebuild the same terrain payload.
    /// Backend-owned quadtree scheduling and complete image coverage remain
    /// separate parity work.
    class NeutralTerrainStorage final : public Terrain::RenderStorage
    {
    public:
        NeutralTerrainStorage(MWWorld::ESMStore& store, const VFS::Manager& vfs,
            std::string_view normalMapPattern = {}, std::string_view normalHeightMapPattern = {},
            bool autoUseNormalMaps = false, std::string_view specularMapPattern = {},
            bool autoUseSpecularMaps = false);
        ~NeutralTerrainStorage() override;

        void fillRenderVertexBuffers(int lodLevel, float size, const std::array<float, 2>& center,
            ESM::RefId worldspace, std::vector<Render::TerrainVertex>& vertices) override;

        void getRenderBlendmaps(float chunkSize, const std::array<float, 2>& chunkCenter,
            std::vector<Render::TextureData>& blendmaps, std::vector<Terrain::LayerInfo>& layerList,
            ESM::RefId worldspace) override;

        float getCellWorldSize(ESM::RefId worldspace) override;
        int getCellVertices(ESM::RefId worldspace) override;
        int getTextureTileCount(float chunkSize, ESM::RefId worldspace) override;

        float getHeightAt(const Render::Vec3& worldPos, ESM::RefId worldspace) override;
        std::optional<Render::TerrainHeightField> getHeightField(
            int gridX, int gridY, ESM::RefId worldspace) override;
        std::optional<Render::TerrainTile> getRenderTile(
            int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace) override;
        void clearCache() override;
        void preloadCells(std::span<const std::array<int, 4>> bounds, ESM::RefId worldspace) override;

    private:
        using Cell = std::pair<int, int>;
        using LayerCache = std::map<VFS::Path::Normalized, Terrain::LayerInfo, std::less<>>;
        using CellCache = std::map<std::tuple<ESM::RefId, int, int>, std::unique_ptr<ESM::LandData>>;
        using RenderTileKey = std::tuple<ESM::RefId, int, float, float, float>;
        struct RenderTileCacheEntry
        {
            std::optional<Render::TerrainTile> tile;
            std::uint64_t lastAccess = 0;
        };
        using RenderTileCache = std::map<RenderTileKey, RenderTileCacheEntry>;

        ESM::RefId resolveLandWorldspace(ESM::RefId worldspace) const;
        std::unique_ptr<ESM::LandData> loadCell(int gridX, int gridY, ESM::RefId worldspace) const;
        const ESM::LandData* getCell(int gridX, int gridY, ESM::RefId worldspace) const;
        Terrain::LayerInfo getLayerInfo(VFS::Path::NormalizedView texture) const;
        Terrain::LayerInfo getEsm4DefaultLayerInfo(int gridX, int gridY, ESM::RefId worldspace) const;
        Terrain::LayerInfo getEsm4LayerInfo(ESM::FormId id) const;
        VFS::Path::Normalized getTextureName(std::uint16_t index, int plugin) const;

        MWWorld::ESMStore& mStore;
        const VFS::Manager& mVfs;
        std::string mNormalMapPattern;
        std::string mNormalHeightMapPattern;
        bool mAutoUseNormalMaps;
        std::string mSpecularMapPattern;
        bool mAutoUseSpecularMaps;
        mutable std::mutex mDataMutex;
        mutable std::mutex mCellCacheMutex;
        mutable CellCache mCellCache;
        mutable std::mutex mRenderTileCacheMutex;
        mutable RenderTileCache mRenderTileCache;
        mutable std::uint64_t mRenderTileCacheGeneration = 0;
        mutable std::uint64_t mRenderTileAccessCounter = 0;
        mutable std::mutex mLayerInfoMutex;
        mutable LayerCache mLayerInfo;
    };
}

#endif
