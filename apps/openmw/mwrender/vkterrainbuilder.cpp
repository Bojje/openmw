#ifdef OPENMW_USE_VULKAN

#include "vkterrainbuilder.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include <components/esm/esmterrain.hpp>
#include <components/esm/path.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadltex.hpp>

#include "../mwbase/environment.hpp"
#include "../mwworld/esmstore.hpp"

namespace
{
    constexpr int sLandSize = ESM::Land::LAND_SIZE; // 65 vertices per side
    constexpr int sQuadsPerSide = sLandSize - 1; // 64
    constexpr int sTextureSize = ESM::Land::LAND_TEXTURE_SIZE; // 16 texture tiles per side
    constexpr int sQuadsPerTile = sQuadsPerSide / sTextureSize; // 4
    constexpr float sVertexSpacing = static_cast<float>(ESM::Land::REAL_SIZE) / sQuadsPerSide; // 128 units

    // position vec3 + normal vec3 + texcoord vec2 + colour vec4
    constexpr std::size_t sFloatsPerVertex = 12;

    constexpr std::string_view sDefaultTexture = "_land_default.dds";

    constexpr int sWantedData
        = ESM::Land::DATA_VHGT | ESM::Land::DATA_VNML | ESM::Land::DATA_VCLR | ESM::Land::DATA_VTEX;

    bool hasData(const ESM::LandData& data, int flag)
    {
        return (data.getLoadFlags() & flag) != 0;
    }

    /// The three cells that share this cell's north and east edges, loaded on demand.
    ///
    /// Constructing an ESM::LandData re-reads the record and fills a ~45 KB LandRecordData, so the
    /// results are kept for the whole build instead of being rebuilt per edge vertex.
    class EdgeNeighbours
    {
    public:
        EdgeNeighbours(const MWWorld::Store<ESM::Land>& store, int cellX, int cellY)
            : mStore(store)
            , mCellX(cellX)
            , mCellY(cellY)
        {
        }

        /// \a offsetX and \a offsetY are each 0 or 1; (0, 0) is this cell and is never asked for.
        /// Returns nullptr when the neighbour has no LAND record.
        const ESM::LandData* get(int offsetX, int offsetY)
        {
            const std::size_t slot = static_cast<std::size_t>(offsetY * 2 + offsetX);
            if (slot == 0)
                return nullptr;

            if (!mSearched[slot])
            {
                mSearched[slot] = true;
                if (const ESM::Land* land = mStore.search(mCellX + offsetX, mCellY + offsetY))
                    mData[slot].emplace(*land, ESM::Land::DATA_VNML | ESM::Land::DATA_VCLR);
            }

            return mData[slot].has_value() ? &*mData[slot] : nullptr;
        }

    private:
        const MWWorld::Store<ESM::Land>& mStore;
        int mCellX;
        int mCellY;
        std::array<std::optional<ESM::LandData>, 4> mData;
        std::array<bool, 4> mSearched{};
    };

    /// Produces the 12 interleaved floats for one grid vertex of a single cell.
    class VertexWriter
    {
    public:
        VertexWriter(const ESM::LandData& land, EdgeNeighbours& neighbours)
            : mLand(land)
            , mNeighbours(neighbours)
        {
        }

        void write(int x, int y, float* out) const
        {
            const std::size_t index = static_cast<std::size_t>(y) * sLandSize + x;

            out[0] = x * sVertexSpacing;
            out[1] = y * sVertexSpacing;
            out[2] = mLand.getHeights()[index];

            // Morrowind writes garbage into the last row and column of the normal array (and
            // occasionally the colour array) because those vertices are duplicates of the first row
            // and column of the neighbouring cell, which holds the real values.
            // ESMTerrain::Storage::fixNormal/fixColour substitute the neighbour's copy; do the same.
            // OpenMW goes further and averages the four surrounding normals at the cell corners
            // (averageNormal), because some corners are garbage in every cell that touches them. Not
            // worth it for a first pass -- the plain substitution removes the visible seam.
            const ESM::LandData* edge = nullptr;
            int edgeX = x;
            int edgeY = y;
            if (x == sQuadsPerSide || y == sQuadsPerSide)
            {
                const int offsetX = x == sQuadsPerSide ? 1 : 0;
                const int offsetY = y == sQuadsPerSide ? 1 : 0;
                edge = mNeighbours.get(offsetX, offsetY);
                edgeX = offsetX != 0 ? 0 : x;
                edgeY = offsetY != 0 ? 0 : y;
            }

            const ESM::LandData* normalSource = &mLand;
            std::size_t normalIndex = index;
            if (edge != nullptr && hasData(*edge, ESM::Land::DATA_VNML))
            {
                normalSource = edge;
                normalIndex = static_cast<std::size_t>(edgeY) * sLandSize + edgeX;
            }

            float nx = 0.0f;
            float ny = 0.0f;
            float nz = 1.0f;
            if (hasData(*normalSource, ESM::Land::DATA_VNML))
            {
                const std::span<const std::int8_t> normals = normalSource->getNormals();
                nx = static_cast<float>(normals[normalIndex * 3]);
                ny = static_cast<float>(normals[normalIndex * 3 + 1]);
                nz = static_cast<float>(normals[normalIndex * 3 + 2]);

                const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (length > 0.0f)
                {
                    nx /= length;
                    ny /= length;
                    nz /= length;
                }
                else
                {
                    nx = 0.0f;
                    ny = 0.0f;
                    nz = 1.0f;
                }
            }

            out[3] = nx;
            out[4] = ny;
            out[5] = nz;

            // A land texture tile spans 4 quads (512 units), so u/v run 0..16 across the cell, which
            // is the tiling vanilla uses.
            out[6] = x / static_cast<float>(sQuadsPerTile);
            out[7] = y / static_cast<float>(sQuadsPerTile);

            const ESM::LandData* colourSource = &mLand;
            std::size_t colourIndex = index;
            if (edge != nullptr && hasData(*edge, ESM::Land::DATA_VCLR))
            {
                colourSource = edge;
                colourIndex = static_cast<std::size_t>(edgeY) * sLandSize + edgeX;
            }

            float r = 1.0f;
            float g = 1.0f;
            float b = 1.0f;
            if (hasData(*colourSource, ESM::Land::DATA_VCLR))
            {
                const std::span<const std::uint8_t> colors = colourSource->getColors();
                r = colors[colourIndex * 3] / 255.0f;
                g = colors[colourIndex * 3 + 1] / 255.0f;
                b = colors[colourIndex * 3 + 2] / 255.0f;
            }

            out[8] = r;
            out[9] = g;
            out[10] = b;
            // Always opaque: the G-buffer fragment shader discards on albedo.a < 0.5 and the vertex
            // colour multiplies into albedo, so anything below 1 here starts punching holes in the
            // ground.
            out[11] = 1.0f;
        }

    private:
        const ESM::LandData& mLand;
        EdgeNeighbours& mNeighbours;
    };

    std::string textureNameAt(const MWWorld::ESMStore& store, const ESM::LandData& land, int tileX, int tileY)
    {
        if (!hasData(land, ESM::Land::DATA_VTEX))
            return std::string(sDefaultTexture);

        const std::uint16_t vtex = land.getTextures()[static_cast<std::size_t>(tileY) * sTextureSize + tileX];
        if (vtex == 0)
            return std::string(sDefaultTexture); // vtex 0 is the base texture regardless of plugin

        // NB: all vtex ids are +1 compared to the ltex ids -- see ESMTerrain::Storage::getTextureName.
        const ESM::Path* path
            = store.get<ESM::LandTexture>().search(static_cast<std::uint32_t>(vtex) - 1, land.getPlugin());
        if (path == nullptr || path->empty())
            return std::string(sDefaultTexture);

        return path->getNormalized().value();
    }
}

namespace MWRender
{
    std::vector<LandChunk> buildLandChunks(int cellX, int cellY)
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const MWWorld::Store<ESM::Land>& landStore = store.get<ESM::Land>();

        const ESM::Land* record = landStore.search(cellX, cellY);
        if (record == nullptr)
            return {};

        const ESM::LandData land(*record, sWantedData);

        // Without heights there is no surface to draw. Normals, colours and textures each have a
        // usable fallback, so they are allowed to be missing.
        if (!hasData(land, ESM::Land::DATA_VHGT) || land.getLandSize() != sLandSize)
            return {};

        EdgeNeighbours neighbours(landStore, cellX, cellY);
        const VertexWriter writer(land, neighbours);

        std::vector<LandChunk> chunks;
        std::vector<std::vector<std::int32_t>> remaps;

        // Grouped by resolved file name rather than by raw vtex id: distinct (index, plugin) pairs can
        // name the same texture, and merging them keeps the draw call count down.
        std::map<std::string, std::size_t, std::less<>> chunkByTexture;
        std::array<std::size_t, sTextureSize * sTextureSize> tileChunk{};

        for (int tileY = 0; tileY < sTextureSize; ++tileY)
        {
            for (int tileX = 0; tileX < sTextureSize; ++tileX)
            {
                std::string texture = textureNameAt(store, land, tileX, tileY);
                auto found = chunkByTexture.find(texture);
                if (found == chunkByTexture.end())
                {
                    found = chunkByTexture.emplace(texture, chunks.size()).first;
                    chunks.emplace_back().texture = std::move(texture);
                    remaps.emplace_back(sLandSize * sLandSize, -1);
                }
                tileChunk[static_cast<std::size_t>(tileY) * sTextureSize + tileX] = found->second;
            }
        }

        const auto vertexIndex = [&](std::size_t chunkIndex, int x, int y) {
            std::int32_t& slot = remaps[chunkIndex][static_cast<std::size_t>(y) * sLandSize + x];
            if (slot < 0)
            {
                LandChunk& chunk = chunks[chunkIndex];
                slot = static_cast<std::int32_t>(chunk.vertices.size() / sFloatsPerVertex);
                chunk.vertices.resize(chunk.vertices.size() + sFloatsPerVertex);
                writer.write(x, y, chunk.vertices.data() + chunk.vertices.size() - sFloatsPerVertex);
            }
            return static_cast<std::uint32_t>(slot);
        };

        for (int y = 0; y < sQuadsPerSide; ++y)
        {
            for (int x = 0; x < sQuadsPerSide; ++x)
            {
                const std::size_t chunkIndex
                    = tileChunk[static_cast<std::size_t>(y / sQuadsPerTile) * sTextureSize + x / sQuadsPerTile];

                const std::uint32_t i00 = vertexIndex(chunkIndex, x, y);
                const std::uint32_t i10 = vertexIndex(chunkIndex, x + 1, y);
                const std::uint32_t i11 = vertexIndex(chunkIndex, x + 1, y + 1);
                const std::uint32_t i01 = vertexIndex(chunkIndex, x, y + 1);

                // The split diagonal alternates per quad, as in Terrain::BufferCache::createIndexBuffer;
                // a fixed diagonal makes slopes look combed in one direction.
                //
                // Winding convention: counter-clockwise seen from +Z looking down, which is what the
                // pipeline's VK_FRONT_FACE_COUNTER_CLOCKWISE plus back-face culling wants for an
                // upward-facing surface in this Z-up, X-east, Y-north world. Each ordering below has a
                // positive Z cross product in the XY plane -- e.g. (i00, i10, i11) walks east then
                // north-east, giving (1,0) x (1,1) = +1 -- so all four triangles face the sky.
                std::vector<std::uint32_t>& indices = chunks[chunkIndex].indices;
                if (((x + y) % 2) == 0)
                {
                    indices.insert(indices.end(), { i00, i10, i11, i00, i11, i01 });
                }
                else
                {
                    indices.insert(indices.end(), { i00, i10, i01, i10, i11, i01 });
                }
            }
        }

        return chunks;
    }
}

#endif
