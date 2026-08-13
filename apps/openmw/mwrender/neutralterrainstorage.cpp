#include "neutralterrainstorage.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include <components/esm/esmterrain.hpp>
#include <components/esm3/loadland.hpp>
#include <components/esm3/loadltex.hpp>
#include <components/esm4/loadland.hpp>
#include <components/esm4/loadltex.hpp>
#include <components/esm4/loadtxst.hpp>
#include <components/esm4/loadwrld.hpp>
#include <components/terrain/gridsampling.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>

#include "../mwworld/esmstore.hpp"
#include "../mwworld/store.hpp"

namespace MWRender
{
    namespace
    {
        constexpr float defaultHeight = static_cast<float>(ESM::Land::DEFAULT_HEIGHT);

        Render::TextureData makeAlphaTexture(int size, const std::vector<std::uint8_t>& alpha)
        {
            if (size <= 0 || alpha.size() != static_cast<std::size_t>(size) * size)
                return {};

            Render::TextureData result;
            result.width = static_cast<std::uint32_t>(size);
            result.height = static_cast<std::uint32_t>(size);
            result.pixels.resize(alpha.size() * 4);
            for (std::size_t i = 0; i < alpha.size(); ++i)
                result.pixels[i * 4 + 3] = alpha[i];
            return result;
        }

        std::array<float, 3> normalize(std::array<float, 3> value)
        {
            const float length = std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
            if (length > 0.f)
                for (float& component : value)
                    component /= length;
            else
                value = { 0.f, 0.f, 1.f };
            return value;
        }

        std::array<float, 3> cross(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
        {
            return { lhs[1] * rhs[2] - lhs[2] * rhs[1], lhs[2] * rhs[0] - lhs[0] * rhs[2],
                lhs[0] * rhs[1] - lhs[1] * rhs[0] };
        }

        std::array<float, 3> subtract(const std::array<float, 3>& lhs, const std::array<float, 3>& rhs)
        {
            return { lhs[0] - rhs[0], lhs[1] - rhs[1], lhs[2] - rhs[2] };
        }
    }

    NeutralTerrainStorage::NeutralTerrainStorage(MWWorld::ESMStore& store, const VFS::Manager& vfs,
        std::string_view normalMapPattern, std::string_view normalHeightMapPattern, bool autoUseNormalMaps,
        std::string_view specularMapPattern, bool autoUseSpecularMaps)
        : mStore(store)
        , mVfs(vfs)
        , mNormalMapPattern(normalMapPattern)
        , mNormalHeightMapPattern(normalHeightMapPattern)
        , mAutoUseNormalMaps(autoUseNormalMaps)
        , mSpecularMapPattern(specularMapPattern)
        , mAutoUseSpecularMaps(autoUseSpecularMaps)
    {
    }

    NeutralTerrainStorage::~NeutralTerrainStorage() = default;

    void NeutralTerrainStorage::clearCache()
    {
        {
            std::lock_guard lock(mCellCacheMutex);
            mCellCache.clear();
        }
        {
            std::lock_guard lock(mRenderTileCacheMutex);
            mRenderTileCache.clear();
        }
    }

    void NeutralTerrainStorage::preloadCells(std::span<const std::array<int, 4>> bounds, ESM::RefId worldspace)
    {
        for (const std::array<int, 4>& range : bounds)
        {
            const int minX = std::min(range[0], range[2]);
            const int maxX = std::max(range[0], range[2]);
            const int minY = std::min(range[1], range[3]);
            const int maxY = std::max(range[1], range[3]);
            for (int y = minY; y < maxY; ++y)
                for (int x = minX; x < maxX; ++x)
                    getCell(x, y, worldspace);
        }
    }

    std::optional<Render::TerrainTile> NeutralTerrainStorage::getRenderTile(
        int lodLevel, float size, const std::array<float, 2>& center, ESM::RefId worldspace)
    {
        const RenderTileKey key{ worldspace, lodLevel, size, center[0], center[1] };
        {
            std::lock_guard lock(mRenderTileCacheMutex);
            const auto found = mRenderTileCache.find(key);
            if (found != mRenderTileCache.end())
                return found->second;
        }

        // Do not hold the cache lock while decoding cells or images. Apart
        // from avoiding lock contention, this lets concurrent requests for
        // different tiles make progress. A duplicate build is harmless; the
        // second result is discarded when the key is inserted below.
        std::optional<Render::TerrainTile> tile = RenderStorage::getRenderTile(lodLevel, size, center, worldspace);
        {
            std::lock_guard lock(mRenderTileCacheMutex);
            const auto [found, inserted] = mRenderTileCache.emplace(key, std::move(tile));
            if (!inserted)
                return found->second;
            return found->second;
        }
    }

    ESM::RefId NeutralTerrainStorage::resolveLandWorldspace(ESM::RefId worldspace) const
    {
        if (!ESM::isEsm4Ext(worldspace))
            return worldspace;

        const ESM4::World* world = mStore.get<ESM4::World>().search(worldspace);
        if (world && !world->mParent.isZeroOrUnset() && world->mParentUseFlags & ESM4::World::UseFlag_Land)
            return world->mParent;
        return worldspace;
    }

    std::unique_ptr<ESM::LandData> NeutralTerrainStorage::loadCell(
        int gridX, int gridY, ESM::RefId worldspace) const
    {
        std::lock_guard lock(mDataMutex);
        worldspace = resolveLandWorldspace(worldspace);
        constexpr int dataFlags = ESM::Land::DATA_VNML | ESM::Land::DATA_VHGT | ESM::Land::DATA_VCLR
            | ESM::Land::DATA_VTEX;

        if (ESM::isEsm4Ext(worldspace))
        {
            const ESM4::Land* land = mStore.get<ESM4::Land>().search({ gridX, gridY, worldspace });
            return land ? std::make_unique<ESM::LandData>(*land, dataFlags) : nullptr;
        }

        const ESM::Land* land = mStore.get<ESM::Land>().search(gridX, gridY);
        return land ? std::make_unique<ESM::LandData>(*land, dataFlags) : nullptr;
    }

    const ESM::LandData* NeutralTerrainStorage::getCell(int gridX, int gridY, ESM::RefId worldspace) const
    {
        worldspace = resolveLandWorldspace(worldspace);
        std::lock_guard lock(mCellCacheMutex);
        const auto key = std::make_tuple(worldspace, gridX, gridY);
        auto found = mCellCache.find(key);
        if (found == mCellCache.end())
            found = mCellCache.emplace(key, loadCell(gridX, gridY, worldspace)).first;
        return found->second.get();
    }

    void NeutralTerrainStorage::fillRenderVertexBuffers(int lodLevel, float size,
        const std::array<float, 2>& center, ESM::RefId worldspace, std::vector<Render::TerrainVertex>& vertices)
    {
        if (lodLevel < 0 || lodLevel > 63)
            throw std::invalid_argument("Invalid terrain lod level: " + std::to_string(lodLevel));
        if (size <= 0.f)
            throw std::invalid_argument("Invalid terrain size: " + std::to_string(size));

        const std::size_t sampleSize = std::size_t{ 1 } << lodLevel;
        const std::size_t cellSize = static_cast<std::size_t>(ESM::getLandSize(worldspace));
        const std::size_t numVerts = static_cast<std::size_t>(size * (cellSize - 1) / sampleSize) + 1;
        vertices.resize(numVerts * numVerts);

        const float cellWorldSize = static_cast<float>(ESM::getCellSize(worldspace));
        const std::array<float, 2> origin = { center[0] - size * 0.5f, center[1] - size * 0.5f };
        const int startCellX = static_cast<int>(std::floor(origin[0]));
        const int startCellY = static_cast<int>(std::floor(origin[1]));
        const auto getNormal = [&](int cellX, int cellY, int col, int row) {
            while (col >= static_cast<int>(cellSize) - 1)
            {
                ++cellY;
                col -= static_cast<int>(cellSize) - 1;
            }
            while (row >= static_cast<int>(cellSize) - 1)
            {
                ++cellX;
                row -= static_cast<int>(cellSize) - 1;
            }
            while (col < 0)
            {
                --cellY;
                col += static_cast<int>(cellSize) - 1;
            }
            while (row < 0)
            {
                --cellX;
                row += static_cast<int>(cellSize) - 1;
            }

            const ESM::LandData* data = getCell(cellX, cellY, worldspace);
            if (!data || !(data->getLoadFlags() & ESM::Land::DATA_VNML))
                return std::array<float, 3>{ 0.f, 0.f, 1.f };
            const std::size_t index = (static_cast<std::size_t>(col) * cellSize + row) * 3;
            const auto normals = data->getNormals();
            return normalize({ static_cast<float>(normals[index]), static_cast<float>(normals[index + 1]),
                static_cast<float>(normals[index + 2]) });
        };

        const auto handleSample = [&](std::size_t cellShiftX, std::size_t cellShiftY, std::size_t row,
                                      std::size_t col, std::size_t vertX, std::size_t vertY) {
            const int cellX = startCellX + static_cast<int>(cellShiftX);
            const int cellY = startCellY + static_cast<int>(cellShiftY);
            const ESM::LandData* data = getCell(cellX, cellY, worldspace);
            const std::size_t index = col * cellSize + row;
            const std::size_t vertexIndex = vertX * numVerts + vertY;

            float height = ESM::isEsm4Ext(worldspace) ? 0.f : defaultHeight;
            if (data && (data->getLoadFlags() & ESM::Land::DATA_VHGT))
                height = data->getHeights()[index];

            vertices[vertexIndex].position = {
                (vertX / static_cast<float>(numVerts - 1) - 0.5f) * size * cellWorldSize,
                (vertY / static_cast<float>(numVerts - 1) - 0.5f) * size * cellWorldSize, height };

            std::array<float, 3> normal = { 0.f, 0.f, 1.f };
            if (data && (data->getLoadFlags() & ESM::Land::DATA_VNML))
            {
                const auto normals = data->getNormals();
                const std::size_t normalIndex = index * 3;
                normal = normalize({ static_cast<float>(normals[normalIndex]), static_cast<float>(normals[normalIndex + 1]),
                    static_cast<float>(normals[normalIndex + 2]) });
            }
            if (col == cellSize - 1 || row == cellSize - 1)
            {
                normal = getNormal(cellX, cellY, static_cast<int>(col), static_cast<int>(row));
                if ((row == 0 || row == cellSize - 1) && (col == 0 || col == cellSize - 1))
                {
                    const auto n1 = getNormal(cellX, cellY, static_cast<int>(col) + 1, static_cast<int>(row));
                    const auto n2 = getNormal(cellX, cellY, static_cast<int>(col) - 1, static_cast<int>(row));
                    const auto n3 = getNormal(cellX, cellY, static_cast<int>(col), static_cast<int>(row) + 1);
                    const auto n4 = getNormal(cellX, cellY, static_cast<int>(col), static_cast<int>(row) - 1);
                    normal = normalize({ n1[0] + n2[0] + n3[0] + n4[0], n1[1] + n2[1] + n3[1] + n4[1],
                        n1[2] + n2[2] + n3[2] + n4[2] });
                }
            }
            vertices[vertexIndex].normal = normal;

            vertices[vertexIndex].color = { 255, 255, 255, 255 };
            if (data && (data->getLoadFlags() & ESM::Land::DATA_VCLR))
            {
                const auto colors = data->getColors();
                const std::size_t colorIndex = index * 3;
                vertices[vertexIndex].color = {
                    colors[colorIndex], colors[colorIndex + 1], colors[colorIndex + 2], 255 };
            }
        };

        const std::size_t beginX = static_cast<std::size_t>((origin[0] - startCellX) * cellSize);
        const std::size_t beginY = static_cast<std::size_t>((origin[1] - startCellY) * cellSize);
        const std::size_t distance = static_cast<std::size_t>(size * (cellSize - 1)) + 1;
        Terrain::sampleCellGrid(cellSize, sampleSize, beginX, beginY, distance, handleSample);
    }

    VFS::Path::Normalized NeutralTerrainStorage::getTextureName(std::uint16_t index, int plugin) const
    {
        constexpr VFS::Path::NormalizedView defaultTexture("_land_default.dds");
        if (index == 0)
            return VFS::Path::Normalized(defaultTexture);

        const ESM::Path* texture = mStore.get<ESM::LandTexture>().search(index - 1, plugin);
        return texture ? Misc::ResourceHelpers::correctTexturePath(texture->getNormalized(), mVfs)
                       : VFS::Path::Normalized(defaultTexture);
    }

    Terrain::LayerInfo NeutralTerrainStorage::getEsm4DefaultLayerInfo(int gridX, int gridY, ESM::RefId worldspace) const
    {
        constexpr VFS::Path::NormalizedView defaultTexture("_land_default.dds");
        worldspace = resolveLandWorldspace(worldspace);
        const ESM4::Land* land = mStore.get<ESM4::Land>().search({ gridX, gridY, worldspace });
        if (!land || land->mDefaultDiffuseMap.empty())
            return getLayerInfo(defaultTexture);

        Terrain::LayerInfo result = getLayerInfo(land->mDefaultDiffuseMap);
        if (!land->mDefaultNormalMap.empty())
            result.mNormalMap = land->mDefaultNormalMap;
        return result;
    }

    Terrain::LayerInfo NeutralTerrainStorage::getEsm4LayerInfo(ESM::FormId id) const
    {
        if (id.isZeroOrUnset())
            return {};

        const ESM4::LandTexture* landTexture = mStore.get<ESM4::LandTexture>().search(id);
        if (!landTexture)
            return {};

        if (!landTexture->mTextureFile.empty())
        {
            constexpr VFS::Path::NormalizedView landscape("textures/landscape");
            return getLayerInfo(VFS::Path::join(landscape, landTexture->mTextureFile));
        }

        const ESM4::TextureSet* textureSet = mStore.get<ESM4::TextureSet>().search(landTexture->mTexture);
        if (!textureSet || textureSet->mDiffuse.empty())
            return {};

        constexpr VFS::Path::NormalizedView textures("textures");
        Terrain::LayerInfo result = getLayerInfo(VFS::Path::join(textures, textureSet->mDiffuse));
        if (!textureSet->mNormalMap.empty())
            result.mNormalMap = VFS::Path::join(textures, textureSet->mNormalMap);
        if (!textureSet->mSpecular.empty())
            result.mSpecularMap = VFS::Path::join(textures, textureSet->mSpecular);
        return result;
    }

    Terrain::LayerInfo NeutralTerrainStorage::getLayerInfo(VFS::Path::NormalizedView texture) const
    {
        std::lock_guard lock(mLayerInfoMutex);
        if (const auto found = mLayerInfo.find(texture); found != mLayerInfo.end())
            return found->second;

        Terrain::LayerInfo info;
        info.mDiffuseMap = texture;
        if (mAutoUseNormalMaps)
        {
            std::string normalHeight(texture.value());
            Misc::StringUtils::replaceLast(normalHeight, ".", mNormalHeightMapPattern + ".");
            VFS::Path::Normalized normalHeightPath(std::move(normalHeight));
            if (mVfs.exists(normalHeightPath))
            {
                info.mNormalMap = normalHeightPath;
                info.mParallax = true;
            }
            else
            {
                std::string normal(texture.value());
                Misc::StringUtils::replaceLast(normal, ".", mNormalMapPattern + ".");
                VFS::Path::Normalized normalPath(std::move(normal));
                if (mVfs.exists(normalPath))
                    info.mNormalMap = normalPath;
            }
        }
        if (mAutoUseSpecularMaps)
        {
            std::string specular(texture.value());
            Misc::StringUtils::replaceLast(specular, ".", mSpecularMapPattern + ".");
            VFS::Path::Normalized specularPath(std::move(specular));
            if (mVfs.exists(specularPath))
                info.mSpecularMap = specularPath;
        }
        mLayerInfo.emplace(texture, info);
        return info;
    }

    void NeutralTerrainStorage::getRenderBlendmaps(float chunkSize, const std::array<float, 2>& chunkCenter,
        std::vector<Render::TextureData>& blendmaps, std::vector<Terrain::LayerInfo>& layerList,
        ESM::RefId worldspace)
    {
        if (ESM::isEsm4Ext(worldspace))
        {
            const std::array<float, 2> origin = { chunkCenter[0] - (chunkSize - 1.f) * 0.5f,
                chunkCenter[1] - (chunkSize + 1.f) * 0.5f };
            constexpr int quadsPerCell = 2;
            constexpr int quadSize = ESM4::Land::sVertsPerSide / quadsPerCell;
            const int blendmapSize = static_cast<int>(chunkSize * quadsPerCell) * quadSize + 1;

            std::map<ESM::FormId, std::size_t> textureIndices;
            std::vector<std::vector<std::uint8_t>> alphaMaps;
            const auto getOrCreateBlendmap = [&](ESM::FormId id, int gridX, int gridY)
                -> std::vector<std::uint8_t>& {
                if (const auto found = textureIndices.find(id); found != textureIndices.end())
                    return alphaMaps[found->second];

                const std::size_t index = alphaMaps.size();
                textureIndices.emplace(id, index);
                alphaMaps.emplace_back(static_cast<std::size_t>(blendmapSize) * blendmapSize, 0);
                layerList.push_back(id.isZeroOrUnset() ? getEsm4DefaultLayerInfo(gridX, gridY, worldspace)
                                                       : getEsm4LayerInfo(id));
                return alphaMaps.back();
            };

            Terrain::sampleBlendmaps(chunkSize, origin[0], origin[1], quadsPerCell,
                [&](const Terrain::CellSample& sample) {
                    const ESM::LandData* data = getCell(sample.mCellX, sample.mCellY, worldspace);
                    if (!data)
                        return;

                    int quad = 0;
                    if (sample.mSrcRow != 0)
                        quad = sample.mSrcCol == 0 ? 1 : 3;
                    else if (sample.mSrcCol != 0)
                        quad = 2;
                    const ESM4::Land::Texture& texture = data->getEsm4Texture(quad);

                    auto& baseBlendmap
                        = getOrCreateBlendmap(ESM::FormId::fromUint32(texture.base.formId), sample.mCellX, sample.mCellY);
                    const int startY = (static_cast<int>(sample.mDstCol) - 1) * quadSize;
                    const int startX = static_cast<int>(sample.mDstRow) * quadSize;
                    for (int y = std::max(0, startY + 1); y <= startY + quadSize && y < blendmapSize; ++y)
                        for (int x = std::max(0, startX); x < startX + quadSize && x < blendmapSize; ++x)
                            baseBlendmap[static_cast<std::size_t>(y) * blendmapSize + x] = 255;

                    for (const auto& layer : texture.layers)
                    {
                        auto& layerBlendmap = getOrCreateBlendmap(
                            ESM::FormId::fromUint32(layer.texture.formId), sample.mCellX, sample.mCellY);
                        for (const ESM4::Land::VTXT& vertex : layer.data)
                        {
                            const int y = vertex.position / (quadSize + 1);
                            const int x = vertex.position % (quadSize + 1);
                            if (x == quadSize || startX + x >= blendmapSize || y == 0 || startY + y >= blendmapSize
                                || startY + y < 0)
                                continue;
                            const std::size_t index = static_cast<std::size_t>((startY + y) * blendmapSize + startX + x);
                            const auto opacity
                                = static_cast<std::uint8_t>(std::clamp(static_cast<int>(vertex.opacity * 255.f), 0, 255));
                            baseBlendmap[index] -= std::min(baseBlendmap[index], opacity);
                            layerBlendmap[index] = opacity;
                        }
                    }
                });

            if (alphaMaps.size() > 1)
                for (const auto& alpha : alphaMaps)
                    blendmaps.push_back(makeAlphaTexture(blendmapSize, alpha));
            return;
        }

        const std::array<float, 2> origin = { chunkCenter[0] - chunkSize * 0.5f,
            chunkCenter[1] - chunkSize * 0.5f };
        constexpr std::size_t imageScaleFactor = 2;
        const std::size_t blendmapSize
            = static_cast<std::size_t>(Terrain::getBlendmapSize(chunkSize, ESM::Land::LAND_TEXTURE_SIZE));
        const std::size_t blendmapImageSize = blendmapSize * imageScaleFactor;

        std::vector<std::pair<std::uint16_t, int>> textureIds(blendmapSize * blendmapSize);
        Terrain::sampleBlendmaps(chunkSize, origin[0], origin[1], ESM::Land::LAND_TEXTURE_SIZE,
            [&](const Terrain::CellSample& sample) {
                const ESM::LandData* data = getCell(sample.mCellX, sample.mCellY, worldspace);
                if (!data || !(data->getLoadFlags() & ESM::Land::DATA_VTEX))
                    return;
                const auto textures = data->getTextures();
                textureIds[sample.mDstCol * blendmapSize + sample.mDstRow]
                    = { textures[sample.mSrcRow * ESM::Land::LAND_TEXTURE_SIZE + sample.mSrcCol], data->getPlugin() };
            });

        std::map<std::pair<std::uint16_t, int>, std::size_t> textureIndices;
        std::vector<std::vector<std::uint8_t>> alphaMaps;
        for (std::size_t y = 0; y < blendmapSize; ++y)
        {
            for (std::size_t x = 0; x < blendmapSize; ++x)
            {
                const auto id = textureIds[y * blendmapSize + x];
                auto found = textureIndices.find(id);
                if (found == textureIndices.end())
                {
                    const std::size_t layerIndex = layerList.size();
                    textureIndices.emplace(id, layerIndex);
                    alphaMaps.emplace_back(blendmapImageSize * blendmapImageSize, 0);
                    layerList.push_back(getLayerInfo(getTextureName(id.first, id.second)));
                    found = textureIndices.find(id);
                }
                auto& alpha = alphaMaps[found->second];
                const std::size_t realY = y * imageScaleFactor;
                const std::size_t realX = x * imageScaleFactor;
                alpha[(realY + 0) * blendmapImageSize + realX + 0] = 255;
                alpha[(realY + 1) * blendmapImageSize + realX + 0] = 255;
                alpha[(realY + 0) * blendmapImageSize + realX + 1] = 255;
                alpha[(realY + 1) * blendmapImageSize + realX + 1] = 255;
            }
        }

        if (alphaMaps.size() <= 1)
            return;
        for (const auto& alpha : alphaMaps)
            blendmaps.push_back(makeAlphaTexture(static_cast<int>(blendmapImageSize), alpha));
    }

    float NeutralTerrainStorage::getCellWorldSize(ESM::RefId worldspace)
    {
        return static_cast<float>(ESM::getCellSize(worldspace));
    }

    int NeutralTerrainStorage::getCellVertices(ESM::RefId worldspace)
    {
        return ESM::getLandSize(worldspace);
    }

    int NeutralTerrainStorage::getTextureTileCount(float chunkSize, ESM::RefId worldspace)
    {
        return ESM::isEsm4Ext(worldspace) ? static_cast<int>(2 * ESM4::Land::sQuadTexturePerSide * chunkSize)
                                          : static_cast<int>(ESM::Land::LAND_TEXTURE_SIZE * chunkSize);
    }

    std::optional<Render::TerrainHeightField> NeutralTerrainStorage::getHeightField(
        int gridX, int gridY, ESM::RefId worldspace)
    {
        const ESM::LandData* data = getCell(gridX, gridY, worldspace);
        if (!data || !(data->getLoadFlags() & ESM::Land::DATA_VHGT))
            return std::nullopt;

        Render::TerrainHeightField result;
        const auto heights = data->getHeights();
        result.heights.assign(heights.begin(), heights.end());
        result.verticesPerSide = static_cast<std::uint32_t>(data->getLandSize());
        result.minHeight = data->getMinHeight();
        result.maxHeight = data->getMaxHeight();
        return result.valid() ? std::optional(std::move(result)) : std::nullopt;
    }

    float NeutralTerrainStorage::getHeightAt(const Render::Vec3& worldPos, ESM::RefId worldspace)
    {
        const float cellSize = getCellWorldSize(worldspace);
        const int cellX = static_cast<int>(std::floor(worldPos.x / cellSize));
        const int cellY = static_cast<int>(std::floor(worldPos.y / cellSize));
        const ESM::LandData* data = getCell(cellX, cellY, worldspace);
        if (!data || !(data->getLoadFlags() & ESM::Land::DATA_VHGT))
            return ESM::isEsm4Ext(worldspace) ? std::numeric_limits<float>::lowest() : defaultHeight;

        const int landSize = data->getLandSize();
        const float nx = (worldPos.x - cellX * cellSize) / cellSize;
        const float ny = (worldPos.y - cellY * cellSize) / cellSize;
        const float factor = static_cast<float>(landSize - 1);
        const int startX = std::min(static_cast<int>(nx * factor), landSize - 1);
        const int startY = std::min(static_cast<int>(ny * factor), landSize - 1);
        const int endX = std::min(startX + 1, landSize - 1);
        const int endY = std::min(startY + 1, landSize - 1);
        const auto heights = data->getHeights();
        const auto height = [&](int x, int y) { return heights[static_cast<std::size_t>(y) * landSize + x] / cellSize; };

        const std::array<float, 3> v0{ startX / factor, startY / factor, height(startX, startY) };
        const std::array<float, 3> v1{ endX / factor, startY / factor, height(endX, startY) };
        const std::array<float, 3> v2{ endX / factor, endY / factor, height(endX, endY) };
        const std::array<float, 3> v3{ startX / factor, endY / factor, height(startX, endY) };
        const float xParam = nx * factor - startX;
        const float yParam = ny * factor - startY;
        const auto planeNormal = (1.f - yParam > xParam) ? cross(subtract(v1, v0), subtract(v3, v0))
                                                           : cross(subtract(v2, v1), subtract(v3, v1));
        const auto& planePoint = (1.f - yParam > xParam) ? v0 : v1;
        if (std::abs(planeNormal[2]) < std::numeric_limits<float>::epsilon())
            return height(startX, startY) * cellSize;
        const float planeD = -(planeNormal[0] * planePoint[0] + planeNormal[1] * planePoint[1]
            + planeNormal[2] * planePoint[2]);
        return (-planeNormal[0] * nx - planeNormal[1] * ny - planeD) / planeNormal[2] * cellSize;
    }
}
