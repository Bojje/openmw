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

#include <components/debug/debuglog.hpp>
#include <components/esm/esmterrain.hpp>
#include <components/esm/path.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadltex.hpp>
#include <components/vk/vkmath.hpp>

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

    /// The eight cells surrounding this one, loaded on demand.
    ///
    /// Constructing an ESM::LandData re-reads the record and fills a ~45 KB LandRecordData, so the
    /// results are kept for the whole build instead of being rebuilt per edge vertex.
    ///
    /// All eight rather than the three to the north and east, because averaging a corner normal
    /// reaches one vertex *back* across the south and west edges as well as forward.
    class EdgeNeighbours
    {
    public:
        EdgeNeighbours(const MWWorld::Store<ESM::Land>& store, int cellX, int cellY)
            : mStore(store)
            , mCellX(cellX)
            , mCellY(cellY)
        {
        }

        /// \a offsetX and \a offsetY are each -1, 0 or 1; (0, 0) is this cell and returns nullptr,
        /// so callers can hand this any offset and fall back to their own data on null. Also null
        /// when the neighbour has no LAND record, which is the map edge and the sea.
        const ESM::LandData* get(int offsetX, int offsetY)
        {
            if (offsetX == 0 && offsetY == 0)
                return nullptr;

            const std::size_t slot = static_cast<std::size_t>((offsetY + 1) * 3 + (offsetX + 1));

            if (!mSearched[slot])
            {
                mSearched[slot] = true;
                if (const ESM::Land* land = mStore.search(mCellX + offsetX, mCellY + offsetY))
                    mData[slot].emplace(
                        *land, ESM::Land::DATA_VNML | ESM::Land::DATA_VCLR | ESM::Land::DATA_VTEX);
            }

            return mData[slot].has_value() ? &*mData[slot] : nullptr;
        }

    private:
        const MWWorld::Store<ESM::Land>& mStore;
        int mCellX;
        int mCellY;
        std::array<std::optional<ESM::LandData>, 9> mData;
        std::array<bool, 9> mSearched{};
    };

    /// Produces the 12 interleaved floats for one grid vertex of a single cell.
    class VertexWriter
    {
    public:
        /// \a uvPerCell selects the texture coordinate convention. False tiles the diffuse once per
        /// land texture tile, which is what a chunk drawn with a raw land texture wants. True runs the
        /// UV 0..1 across the whole cell, which is what a chunk drawn with a baked composite wants --
        /// the composite already has the tiling painted into it.
        VertexWriter(const ESM::LandData& land, EdgeNeighbours& neighbours, bool uvPerCell)
            : mLand(land)
            , mNeighbours(neighbours)
            , mUvPerCell(uvPerCell)
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
            // and column of the neighbouring cell, which holds the real values. normalAt substitutes
            // the neighbour's copy, the way ESMTerrain::Storage::fixNormal does.
            //
            // The four cell corners need more than that: they are garbage in *every* cell that
            // touches them, so there is no neighbour holding a good value to substitute. OpenMW
            // averages the four surrounding vertices instead (Storage::averageNormal) and so does
            // this. Four vertices a cell, and without it each corner is a visible shading spike.
            float nx = 0.0f;
            float ny = 0.0f;
            float nz = 1.0f;

            const bool isCorner = (x == 0 || x == sQuadsPerSide) && (y == 0 || y == sQuadsPerSide);
            if (isCorner)
            {
                float sum[3] = { 0.0f, 0.0f, 0.0f };
                const int offsets[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
                for (const auto& offset : offsets)
                {
                    float neighbour[3];
                    normalAt(x + offset[0], y + offset[1], neighbour);
                    sum[0] += neighbour[0];
                    sum[1] += neighbour[1];
                    sum[2] += neighbour[2];
                }
                normalise(sum, nx, ny, nz);
            }
            else
            {
                float own[3];
                normalAt(x, y, own);
                nx = own[0];
                ny = own[1];
                nz = own[2];
            }

            out[3] = nx;
            out[4] = ny;
            out[5] = nz;

            // A land texture tile spans 4 quads (512 units), so u/v run 0..16 across the cell, which
            // is the tiling vanilla uses. A composite has that tiling baked in already and wants 0..1.
            const float uvDivisor
                = mUvPerCell ? static_cast<float>(sQuadsPerSide) : static_cast<float>(sQuadsPerTile);
            out[6] = x / uvDivisor;
            out[7] = y / uvDivisor;

            // Colour takes the plain substitution only. The corner averaging above exists because a
            // corner *normal* is garbage in every cell that touches it; the colours there are fine.
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
                // Decoded to linear, like every other authored colour that enters this renderer.
                //
                // VCLR is the per-vertex terrain tint and it was authored by artists looking at
                // gamma-space compositing -- the OSG renderer lights in gamma space throughout. The
                // G-buffer fragment shader multiplies this straight into an albedo that *has* been
                // decoded, because the textures are uploaded as _SRGB block formats (trap 8). So the
                // two factors of that product were in different spaces, which systematically
                // brightened and desaturated every shaded patch of ground.
                const std::span<const std::uint8_t> colors = colourSource->getColors();
                r = Vk::srgbToLinear(colors[colourIndex * 3] / 255.0f);
                g = Vk::srgbToLinear(colors[colourIndex * 3 + 1] / 255.0f);
                b = Vk::srgbToLinear(colors[colourIndex * 3 + 2] / 255.0f);
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
        static void normalise(const float in[3], float& x, float& y, float& z)
        {
            const float length = std::sqrt(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
            if (length > 0.0f)
            {
                x = in[0] / length;
                y = in[1] / length;
                z = in[2] / length;
            }
            else
            {
                // Straight up. A zero normal is not a direction and shading it as one puts a black
                // spot on the ground; ESMTerrain::Storage::fixNormal makes the same substitution.
                x = 0.0f;
                y = 0.0f;
                z = 1.0f;
            }
        }

        /// The normal at a grid coordinate, which may be one vertex outside this cell in any
        /// direction. Corner averaging needs that, and the last row and column need it anyway
        /// because Morrowind fills them with garbage.
        ///
        /// A cell's vertex 64 is the same point as the next cell's vertex 0, so stepping across
        /// costs 64 rather than 65 -- coordinate 65 is the neighbour's vertex 1, and -1 is the
        /// previous neighbour's vertex 63. Getting that off by one puts the sample a whole quad
        /// away, which is 128 world units and looks like noise rather than like an indexing bug.
        void normalAt(int x, int y, float out[3]) const
        {
            int offsetX = 0;
            int offsetY = 0;
            if (x < 0)
            {
                offsetX = -1;
                x += sQuadsPerSide;
            }
            else if (x >= sQuadsPerSide)
            {
                offsetX = 1;
                x -= sQuadsPerSide;
            }

            if (y < 0)
            {
                offsetY = -1;
                y += sQuadsPerSide;
            }
            else if (y >= sQuadsPerSide)
            {
                offsetY = 1;
                y -= sQuadsPerSide;
            }

            const ESM::LandData* source = &mLand;
            if (offsetX != 0 || offsetY != 0)
            {
                const ESM::LandData* neighbour = mNeighbours.get(offsetX, offsetY);
                if (neighbour != nullptr && hasData(*neighbour, ESM::Land::DATA_VNML))
                {
                    source = neighbour;
                }
                else
                {
                    // No neighbour -- the map edge, or open sea with no LAND record. Clamp back into
                    // this cell rather than reading past the end of the array.
                    x = std::clamp(x, 0, sQuadsPerSide);
                    y = std::clamp(y, 0, sQuadsPerSide);
                }
            }

            out[0] = 0.0f;
            out[1] = 0.0f;
            out[2] = 1.0f;
            if (!hasData(*source, ESM::Land::DATA_VNML))
                return;

            const std::size_t index = static_cast<std::size_t>(y) * sLandSize + x;
            const std::span<const std::int8_t> normals = source->getNormals();
            const float raw[3] = {
                static_cast<float>(normals[index * 3]),
                static_cast<float>(normals[index * 3 + 1]),
                static_cast<float>(normals[index * 3 + 2]),
            };
            normalise(raw, out[0], out[1], out[2]);
        }

        const ESM::LandData& mLand;
        EdgeNeighbours& mNeighbours;
        bool mUvPerCell = false;
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

    /// The land texture at a tile that may lie one cell outside this one.
    ///
    /// A missing neighbour is the base texture rather than a hole -- ESMTerrain::Storage's
    /// getTextureIdAt returns {0, 0} for absent land or absent VTEX, and the borrowed samples along a
    /// map edge or an open coast depend on that.
    std::string textureNameAcross(const MWWorld::ESMStore& store, const ESM::LandData& land,
        EdgeNeighbours& neighbours, int cellOffsetX, int cellOffsetY, int tileX, int tileY)
    {
        if (cellOffsetX == 0 && cellOffsetY == 0)
            return textureNameAt(store, land, tileX, tileY);

        const ESM::LandData* neighbour = neighbours.get(cellOffsetX, cellOffsetY);
        if (neighbour == nullptr)
            return std::string(sDefaultTexture);

        return textureNameAt(store, *neighbour, tileX, tileY);
    }

    /// The per-layer alpha maps that turn the hard 16x16 VTEX grid into the soft transitions OSG
    /// shows. Reimplemented from ESMTerrain::Storage::getBlendmaps rather than shared, because the
    /// Vulkan path deliberately does not depend on components/terrain.
    ///
    /// Everything about this is a vertex-centred grid over a texel-centred paint map, and the
    /// off-by-ones are not symmetric. Four things are easy to get wrong and only one of them shows
    /// up as an obvious error:
    ///
    ///   - The grid is 17 samples across for 16 texels. The extra sample is at the LOW end in X,
    ///     borrowed from the cell to the west, and at the HIGH end in Y, borrowed from the cell to
    ///     the north. Putting both extras on the same side shifts the whole map by half a texel.
    ///   - VTEX is indexed [y * 16 + x]. Upstream calls the X axis "row" and the Y axis "col"
    ///     throughout this subsystem, which is the opposite of the usual convention and is the
    ///     single easiest way to produce a transposed map that still looks plausible.
    ///   - Each sample is written as a 2x2 block into a doubled image. The doubling is not a quality
    ///     choice: the quarter-texel nudge in the sampling UV is exactly half a doubled pixel and is
    ///     not representable without it.
    ///   - The weights sum to exactly 1 everywhere, because each sample writes 255 into exactly one
    ///     layer and 0 stays everywhere else. The consumer must therefore composite as a weighted
    ///     sum, not as alpha-over.
    MWRender::LandBlend buildBlendMaps(
        const MWWorld::ESMStore& store, const ESM::LandData& land, EdgeNeighbours& neighbours)
    {
        using MWRender::sBlendImageSize;
        using MWRender::sBlendSamples;

        MWRender::LandBlend blend;

        std::array<std::size_t, sBlendSamples * sBlendSamples> sampleLayer{};
        std::map<std::string, std::size_t, std::less<>> layerByTexture;

        for (int j = 0; j < sBlendSamples; ++j) // j is world Y
        {
            // The extra Y sample is the far one, and it reads the north neighbour's first texel row.
            const int cellOffsetY = j == sTextureSize ? 1 : 0;
            const int srcY = j == sTextureSize ? 0 : j;

            for (int i = 0; i < sBlendSamples; ++i) // i is world X
            {
                // The extra X sample is the near one, and it reads the west neighbour's last texel
                // column. Opposite end from Y, which is not a mistake -- see the note above.
                const int cellOffsetX = i == 0 ? -1 : 0;
                const int srcX = i == 0 ? sTextureSize - 1 : i - 1;

                std::string texture
                    = textureNameAcross(store, land, neighbours, cellOffsetX, cellOffsetY, srcX, srcY);

                auto found = layerByTexture.find(texture);
                if (found == layerByTexture.end())
                {
                    found = layerByTexture.emplace(texture, blend.layers.size()).first;
                    blend.layers.push_back(std::move(texture));
                }

                sampleLayer[static_cast<std::size_t>(j) * sBlendSamples + i] = found->second;
            }
        }

        // One texture over the whole cell needs no blending at all, and saying so lets the caller
        // skip the bake entirely and point the chunk straight at that texture.
        if (blend.layers.size() <= 1)
            return blend;

        blend.maps.assign(blend.layers.size(), std::vector<std::uint8_t>(sBlendImageSize * sBlendImageSize, 0));

        for (int j = 0; j < sBlendSamples; ++j)
        {
            for (int i = 0; i < sBlendSamples; ++i)
            {
                std::vector<std::uint8_t>& map
                    = blend.maps[sampleLayer[static_cast<std::size_t>(j) * sBlendSamples + i]];

                const std::size_t x = static_cast<std::size_t>(i) * 2;
                const std::size_t y = static_cast<std::size_t>(j) * 2;
                map[(y + 0) * sBlendImageSize + x + 0] = 255;
                map[(y + 0) * sBlendImageSize + x + 1] = 255;
                map[(y + 1) * sBlendImageSize + x + 0] = 255;
                map[(y + 1) * sBlendImageSize + x + 1] = 255;
            }
        }

        return blend;
    }
}

namespace MWRender
{
    LandBlend buildLandBlend(int cellX, int cellY)
    {
        const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
        const MWWorld::Store<ESM::Land>& landStore = store.get<ESM::Land>();

        const ESM::Land* record = landStore.search(cellX, cellY);
        if (record == nullptr)
            return {};

        const ESM::LandData land(*record, sWantedData);
        EdgeNeighbours neighbours(landStore, cellX, cellY);
        LandBlend blend = buildBlendMaps(store, land, neighbours);

        return blend;
    }

    std::vector<LandChunk> buildLandChunks(int cellX, int cellY, bool oneChunk)
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
        const VertexWriter writer(land, neighbours, oneChunk);

        std::vector<LandChunk> chunks;
        std::vector<std::vector<std::int32_t>> remaps;

        std::array<std::size_t, sTextureSize * sTextureSize> tileChunk{};

        if (oneChunk)
        {
            // One chunk for the whole cell, drawn with a composite the caller bakes. tileChunk stays
            // all zeroes, so every quad lands in it. `texture` is left empty deliberately: there is no
            // single land texture that describes this chunk, and the caller must supply the composite.
            chunks.emplace_back();
            remaps.emplace_back(sLandSize * sLandSize, -1);
        }
        else
        {
            // Grouped by resolved file name rather than by raw vtex id: distinct (index, plugin) pairs
            // can name the same texture, and merging them keeps the draw call count down.
            std::map<std::string, std::size_t, std::less<>> chunkByTexture;

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
