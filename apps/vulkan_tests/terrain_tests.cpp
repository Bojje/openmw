#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <osg/Image>

#include <components/terrain/storage.hpp>
#include <components/render/terrainpaging.hpp>
#include <components/render/terrainmesh.hpp>

namespace
{
    class TestStorage final : public Terrain::Storage
    {
    public:
        bool mOpaqueOnly = false;
        bool mEmpty = false;

        void getBounds(float&, float&, float&, float&, ESM::RefId) override {}

        bool getMinMaxHeights(float, const osg::Vec2f&, ESM::RefId, float&, float&) override { return true; }

        void fillRenderVertexBuffers(int, float, const std::array<float, 2>&, ESM::RefId,
            std::vector<Render::TerrainVertex>& vertices) override
        {
            if (mEmpty)
                return;

            vertices = {
                { { 0.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 255, 0, 0, 255 } },
                { { 1.f, 0.f, 1.f }, { 0.f, 0.f, 1.f }, { 0, 255, 0, 255 } },
                { { 0.f, 1.f, 1.f }, { 0.f, 0.f, 1.f }, { 0, 0, 255, 255 } },
                { { 1.f, 1.f, 1.f }, { 0.f, 0.f, 1.f }, { 255, 255, 255, 255 } },
            };
        }

        void getRenderBlendmaps(float, const std::array<float, 2>&, std::vector<Render::TextureData>& blendmaps,
            std::vector<Terrain::LayerInfo>& layers, ESM::RefId) override
        {
            if (!mOpaqueOnly)
            {
                Render::TextureData blendmap;
                blendmap.width = 2;
                blendmap.height = 1;
                blendmap.pixels = { 255, 128, 64, 191, 0, 64, 128, 255 };
                blendmaps.push_back(std::move(blendmap));
            }

            Terrain::LayerInfo layer;
            layer.mDiffuseMap = VFS::Path::Normalized("textures/grass.dds");
            layer.mNormalMap = VFS::Path::Normalized("textures/grass_n.dds");
            layer.mSpecularMap = VFS::Path::Normalized("textures/grass_spec.dds");
            layer.mParallax = true;
            layer.mSpecular = true;
            layers.push_back(std::move(layer));
        }

        float getHeightAt(const osg::Vec3f&, ESM::RefId) override { return 1.f; }
        float getCellWorldSize(ESM::RefId) override { return 1.f; }
        int getCellVertices(ESM::RefId) override { return 3; }
        int getTextureTileCount(float, ESM::RefId) override { return 1; }
    };

    void expect(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        Terrain::LayerInfo defaultLayer;
        expect(!defaultLayer.mParallax && !defaultLayer.mSpecular,
            "terrain layer feature flags must default to disabled");

        Render::TerrainHeightField heightField;
        heightField.verticesPerSide = 2;
        heightField.heights = { -1.f, 0.f, 1.f, 2.f };
        heightField.minHeight = -1.f;
        heightField.maxHeight = 2.f;
        expect(heightField.valid(), "neutral terrain heightfield data should validate");
        heightField.heights.pop_back();
        expect(!heightField.valid(), "neutral terrain heightfield must reject incomplete samples");

        TestStorage storage;
        Terrain::RenderStorage& neutralStorage = storage;
        const auto tile = neutralStorage.getRenderTile(2, 4.f, { 3.f, -2.f }, ESM::RefId());
        const auto cellLodTiles = neutralStorage.getRenderTiles(3, -2, ESM::RefId());
        expect(cellLodTiles.size() == 2 && cellLodTiles[0].lod == 0 && cellLodTiles[1].lod == 1
                && cellLodTiles[0].center[0] == 3.5f && cellLodTiles[0].center[1] == -1.5f,
            "neutral terrain storage did not assemble deterministic cell LOD snapshots");
        const auto regionTiles = neutralStorage.getRenderRegionTiles(0, 3, 0, 1, ESM::RefId());
        expect(regionTiles.size() == 2 && regionTiles[0].minCellX == 0 && regionTiles[0].maxCellX == 1
                && regionTiles[0].minCellY == 0 && regionTiles[0].maxCellY == 1 && regionTiles[1].minCellX == 2
                && regionTiles[1].maxCellX == 3 && regionTiles[1].minCellY == 0 && regionTiles[1].maxCellY == 1
                && regionTiles[0].lods.size() == 2 && regionTiles[0].lods[0].size == 2.f
                && regionTiles[0].lods[0].center[0] == 1.f && regionTiles[0].lods[0].center[1] == 1.f
                && regionTiles[0].lods[1].lod == 1 && regionTiles[0].valid() && regionTiles[1].valid(),
            "neutral terrain storage did not assemble aligned region LOD snapshots");
        expect(tile.has_value() && tile->valid(), "terrain adapter returned an invalid tile");
        expect(tile->lod == 2 && tile->size == 4.f && tile->center[0] == 3.f && tile->center[1] == -2.f
                && tile->cellWorldSize == 1.f,
            "terrain tile metadata was not preserved");
        expect(tile->verticesPerSide == 2 && tile->vertices.size() == 4 && tile->indices.size() == 6
                && tile->indices[0] == 0 && tile->indices[5] == 3 && tile->vertices[1].position[0] == 1.f
                && tile->vertices[2].color[2] == 255,
            "terrain vertices were not converted");
        expect(tile->layers.size() == 1 && tile->layers[0].diffuseTexture == "textures/grass.dds"
                && tile->layers[0].normalTexture == "textures/grass_n.dds"
                && tile->layers[0].specularTexture == "textures/grass_spec.dds" && tile->layers[0].parallax
                && tile->layers[0].specular,
            "terrain layer metadata was not converted");
        expect(tile->layers[0].blendmap.valid() && tile->layers[0].blendmap.pixels[3] == 191,
            "terrain blendmap was not converted to RGBA8");

        osg::ref_ptr<osg::Vec3Array> legacyPositions = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec3Array> legacyNormals = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4ubArray> legacyColors = new osg::Vec4ubArray;
        storage.fillVertexBuffers(2, 4.f, osg::Vec2f(3.f, -2.f), ESM::RefId(), *legacyPositions, *legacyNormals,
            *legacyColors);
        expect(legacyPositions->size() == 4 && legacyNormals->size() == 4 && legacyColors->size() == 4
                && (*legacyPositions)[1].x() == 1.f && (*legacyColors)[2].b() == 255,
            "legacy terrain adapter did not preserve neutral vertices");

        Terrain::Storage::ImageVector legacyBlendmaps;
        std::vector<Terrain::LayerInfo> legacyLayers;
        storage.getBlendmaps(4.f, osg::Vec2f(3.f, -2.f), legacyBlendmaps, legacyLayers, ESM::RefId());
        expect(legacyBlendmaps.size() == 1 && legacyLayers.size() == 1 && legacyBlendmaps.front()->data()[3] == 191,
            "legacy terrain adapter did not preserve neutral blendmaps");

        storage.mOpaqueOnly = true;
        const auto opaqueTile = neutralStorage.getRenderTile(0, 1.f, { 0.f, 0.f }, ESM::RefId());
        expect(opaqueTile.has_value() && opaqueTile->valid() && opaqueTile->layers.size() == 1
                && !opaqueTile->layers[0].blendmap.valid(),
            "opaque terrain layer should not require a blendmap");

        storage.mEmpty = true;
        expect(!neutralStorage.getRenderTile(0, 1.f, { 0.f, 0.f }, ESM::RefId()).has_value(),
            "empty terrain storage should not produce a tile");
        storage.mEmpty = false;
        const auto opaqueTerrainMeshes = Render::makeTerrainMeshes(*opaqueTile);
        expect(opaqueTerrainMeshes.size() == 1 && opaqueTerrainMeshes.front().mesh.indices == opaqueTile->indices
                && opaqueTerrainMeshes.front().mesh.material.albedoTexture == "textures/grass.dds"
                && opaqueTerrainMeshes.front().transform.data[12] == 0.f
                && opaqueTerrainMeshes.front().mesh.vertices[3].texcoord[0] == 1.f
                && opaqueTerrainMeshes.front().mesh.vertices[3].texcoord[1] == 1.f,
            "opaque terrain tile was not converted for Vulkan mesh submission");
        const auto blendedTerrainMeshes = Render::makeTerrainMeshes(*tile);
        expect(blendedTerrainMeshes.size() == 1 && blendedTerrainMeshes.front().mesh.material.terrainBlend
                && blendedTerrainMeshes.front().mesh.material.terrainNormalMap
                && blendedTerrainMeshes.front().mesh.material.terrainParallax
                && blendedTerrainMeshes.front().mesh.material.terrainSpecular
                && blendedTerrainMeshes.front().mesh.material.normalTexture == "textures/grass_n.dds"
                && blendedTerrainMeshes.front().mesh.material.specularTexture == "textures/grass_spec.dds"
                && blendedTerrainMeshes.front().mesh.material.alphaTexture
                && blendedTerrainMeshes.front().mesh.material.alphaTexture->valid()
                && blendedTerrainMeshes.front().mesh.vertices[3].texcoord[0] > 3.99f
                && blendedTerrainMeshes.front().mesh.vertices[3].texcoord[0] < 4.01f
                && blendedTerrainMeshes.front().mesh.vertices[3].blendTexcoord[0] > 0.99f
                && blendedTerrainMeshes.front().mesh.vertices[3].blendTexcoord[0] < 1.01f
                && blendedTerrainMeshes.front().mesh.vertices[3].blendTexcoord[1] > 0.49f
                && blendedTerrainMeshes.front().mesh.vertices[3].blendTexcoord[1] < 0.51f,
            "terrain blendmap layer was not converted for Vulkan submission");
        Render::TerrainTile multiLayerTile = *tile;
        multiLayerTile.layers.push_back(tile->layers.front());
        const auto multiLayerMeshes = Render::makeTerrainMeshes(multiLayerTile);
        expect(multiLayerMeshes.size() == 2 && multiLayerMeshes[0].mesh.material.terrainFirstLayer
                && !multiLayerMeshes[1].mesh.material.terrainFirstLayer,
            "terrain layers were not kept in ordered first/subsequent form");

        Render::TerrainTile missingMultiLayerBlendmap = *opaqueTile;
        missingMultiLayerBlendmap.layers.push_back(opaqueTile->layers.front());
        expect(!missingMultiLayerBlendmap.valid(), "multi-layer terrain without blendmaps should be rejected");

        Render::TerrainTile malformedBlendmap = *tile;
        malformedBlendmap.layers.front().blendmap.pixels.pop_back();
        expect(!malformedBlendmap.valid(), "terrain with malformed blendmap data should be rejected");

        Render::TerrainTile malformed = *opaqueTile;
        malformed.indices.back() = static_cast<std::uint32_t>(malformed.vertices.size());
        expect(!malformed.valid() && Render::makeTerrainMeshes(malformed).empty(),
            "terrain validation should reject out-of-range indices");

        Render::TerrainTile lodOne = *opaqueTile;
        lodOne.lod = 1;
        Render::TerrainTile lodTwo = *opaqueTile;
        lodTwo.lod = 2;
        const std::vector<Render::TerrainTile> lodTiles = { *opaqueTile, lodOne, lodTwo };
        expect(Render::selectTerrainLod(lodTiles, 0.f, 0.f)->lod == 0
                && Render::selectTerrainLod(lodTiles, 40.f, 0.f)->lod == 1
                && Render::selectTerrainLod(lodTiles, 80.f, 0.f)->lod == 2
                && Render::selectTerrainLod(lodTiles, 80.f, 0.f, 1)->lod == 1,
            "terrain LOD selection did not follow deterministic distance thresholds");

        std::cout << "Vulkan terrain snapshot tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan terrain snapshot test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
