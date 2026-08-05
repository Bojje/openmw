#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

#include <osg/Image>

#include <components/terrain/storage.hpp>
#include <components/render/terrainmesh.hpp>

namespace
{
    class TestStorage final : public Terrain::Storage
    {
    public:
        bool mOpaqueOnly = false;

        void getBounds(float&, float&, float&, float&, ESM::RefId) override {}

        bool getMinMaxHeights(float, const osg::Vec2f&, ESM::RefId, float&, float&) override { return true; }

        void fillVertexBuffers(int, float, const osg::Vec2f&, ESM::RefId, osg::Vec3Array& positions,
            osg::Vec3Array& normals, osg::Vec4ubArray& colors) override
        {
            positions.push_back(osg::Vec3f(0.f, 0.f, 1.f));
            positions.push_back(osg::Vec3f(1.f, 0.f, 1.f));
            positions.push_back(osg::Vec3f(0.f, 1.f, 1.f));
            positions.push_back(osg::Vec3f(1.f, 1.f, 1.f));
            normals.push_back(osg::Vec3f(0.f, 0.f, 1.f));
            normals.push_back(osg::Vec3f(0.f, 0.f, 1.f));
            normals.push_back(osg::Vec3f(0.f, 0.f, 1.f));
            normals.push_back(osg::Vec3f(0.f, 0.f, 1.f));
            colors.push_back(osg::Vec4ub(255, 0, 0, 255));
            colors.push_back(osg::Vec4ub(0, 255, 0, 255));
            colors.push_back(osg::Vec4ub(0, 0, 255, 255));
            colors.push_back(osg::Vec4ub(255, 255, 255, 255));
        }

        void getBlendmaps(float, const osg::Vec2f&, ImageVector& blendmaps, std::vector<Terrain::LayerInfo>& layers,
            ESM::RefId) override
        {
            if (!mOpaqueOnly)
            {
                osg::ref_ptr<osg::Image> blendmap = new osg::Image;
                blendmap->allocateImage(2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE);
                blendmap->setColor(osg::Vec4(1.f, 0.5f, 0.25f, 0.75f), 0, 0);
                blendmap->setColor(osg::Vec4(0.f, 0.25f, 0.5f, 1.f), 1, 0);
                blendmaps.push_back(blendmap);
            }

            Terrain::LayerInfo layer;
            layer.mDiffuseMap = VFS::Path::Normalized("textures/grass.dds");
            layer.mNormalMap = VFS::Path::Normalized("textures/grass_n.dds");
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
        TestStorage storage;
        const auto tile = storage.getRenderTile(2, 4.f, osg::Vec2f(3.f, -2.f), ESM::RefId());
        expect(tile.has_value() && tile->valid(), "terrain adapter returned an invalid tile");
        expect(tile->lod == 2 && tile->size == 4.f && tile->center[0] == 3.f && tile->center[1] == -2.f
                && tile->cellWorldSize == 1.f,
            "terrain tile metadata was not preserved");
        expect(tile->verticesPerSide == 2 && tile->vertices.size() == 4 && tile->indices.size() == 6
                && tile->indices[0] == 0 && tile->indices[5] == 3 && tile->vertices[1].position[0] == 1.f
                && tile->vertices[2].color[2] == 255,
            "terrain vertices were not converted");
        expect(tile->layers.size() == 1 && tile->layers[0].diffuseTexture == "textures/grass.dds"
                && tile->layers[0].normalTexture == "textures/grass_n.dds" && tile->layers[0].parallax
                && tile->layers[0].specular,
            "terrain layer metadata was not converted");
        expect(tile->layers[0].blendmap.valid() && tile->layers[0].blendmap.pixels[3] == 191,
            "terrain blendmap was not converted to RGBA8");

        storage.mOpaqueOnly = true;
        const auto opaqueTile = storage.getRenderTile(0, 1.f, osg::Vec2f(), ESM::RefId());
        expect(opaqueTile.has_value() && opaqueTile->valid() && opaqueTile->layers.size() == 1
                && !opaqueTile->layers[0].blendmap.valid(),
            "opaque terrain layer should not require a blendmap");
        const auto terrainMesh = Render::makeOpaqueTerrainMesh(*opaqueTile);
        expect(terrainMesh.has_value() && terrainMesh->mesh.indices == opaqueTile->indices
                && terrainMesh->mesh.material.albedoTexture == "textures/grass.dds"
                && terrainMesh->transform.data[12] == 0.f && terrainMesh->mesh.vertices[3].texcoord[0] == 1.f
                && terrainMesh->mesh.vertices[3].texcoord[1] == 1.f,
            "opaque terrain tile was not converted for Vulkan mesh submission");
        expect(!Render::makeOpaqueTerrainMesh(*tile).has_value(),
            "multi-layer terrain tile should wait for a terrain shader consumer");

        std::cout << "Vulkan terrain snapshot tests passed\n";
        return EXIT_SUCCESS;
    }
    catch (const std::exception& error)
    {
        std::cerr << "Vulkan terrain snapshot test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
