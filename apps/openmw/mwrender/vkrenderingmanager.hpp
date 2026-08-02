#ifndef OPENMW_MWRENDER_VKRENDERINGMANAGER_H
#define OPENMW_MWRENDER_VKRENDERINGMANAGER_H

#ifdef OPENMW_USE_VULKAN

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <osg/Vec3f>

// Vk::Geometry is held by value in CellTerrain, so unlike the other Vk types it cannot be forward
// declared here.
#include <components/vk/vkgeometry.hpp>

struct SDL_Window;

namespace Vk
{
    class Renderer;
    class Texture;
    struct Mat4;
    struct SceneData;
}

namespace NifVk
{
    struct VulkanMesh;
    class MeshConverter;
}

namespace MWWorld
{
    class CellStore;
}

namespace MWRender
{
    class Camera;

    class VkRenderingManager
    {
    public:
        VkRenderingManager(SDL_Window* window, bool enableValidation);
        ~VkRenderingManager();

        /// \a sunLightDir is the direction the sunlight *travels* (pointing away from the sun), which is
        /// the negation of World::getSunLightPosition(). It need not be normalised.
        void render(Camera& camera, const osg::Vec3f& sunLightDir);
        bool loadShaders(const std::filesystem::path& shaderDir);

        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);

        // Brings the loaded cell set in line with the OSG side's active cells. Called once per frame so
        // the Vulkan renderer stays a passive observer of the world rather than requiring hooks in
        // MWWorld::Scene. The type matches MWWorld::Scene::CellStoreCollection.
        void syncCells(const std::set<MWWorld::CellStore*, std::less<>>& activeCells);

        void resize(uint32_t width, uint32_t height);

    private:
        struct CellMeshes
        {
            struct Instance
            {
                size_t meshIndex;
                float transform[16];
            };
            std::vector<Instance> instances;
        };

        // Terrain is stored per cell rather than in the shared mMeshes table because, unlike object
        // meshes, a heightfield is unique to its cell and is never reused. Holding it here lets
        // removeCell() free it, which matters once the player walks: a cell's terrain is a few hundred
        // KB of device memory and the cell grid churns continuously.
        struct CellTerrain
        {
            struct Chunk
            {
                Vk::Geometry geometry;
                size_t textureIndex; // index into mTextures, or npos when untextured
            };
            std::vector<Chunk> chunks;
            // Cell-local vertices plus one translation to the cell origin, so terrain coordinates stay
            // small instead of running out to the ±250,000 units the world spans.
            float transform[16];
        };

        // A single NIF yields several submeshes, so the cache maps a model path to all of the
        // mMeshes indices it produced. Returns nullptr only if the model could not be loaded.
        const std::vector<size_t>* getOrLoadMeshes(const std::string& model);

        // Builds and uploads the heightfield for an exterior cell. Does nothing for interiors or for
        // exteriors with no LAND record.
        void addTerrain(const MWWorld::CellStore* store);

        // Resolves a raw NIF texture name to an index into mTextures, loading it if needed. Returns
        // npos when the texture cannot be loaded, so callers can fall back to untextured rendering.
        size_t getOrLoadTexture(const std::string& nifTextureName);

        std::unique_ptr<Vk::Renderer> mRenderer;
        std::unique_ptr<NifVk::MeshConverter> mMeshConverter;

        std::unordered_map<std::string, std::vector<size_t>> mMeshCache;
        std::vector<std::unique_ptr<NifVk::VulkanMesh>> mMeshes;

        // Parallel to mMeshes: the texture index each mesh draws with, or npos if untextured.
        // Kept here rather than on NifVk::VulkanMesh so the NIF converter stays unaware of the
        // renderer's texture table.
        std::vector<size_t> mMeshTextures;

        // Pushes the current mTextures views into the renderer's sampler array. Cheap to call when
        // nothing changed; does nothing unless mTextures has grown.
        void syncTexturesToRenderer();

        std::unordered_map<std::string, size_t> mTextureCache;
        std::vector<std::unique_ptr<Vk::Texture>> mTextures;
        // How many of mTextures the renderer's descriptor array has been told about.
        size_t mTexturesUploaded = 0;

        std::unordered_map<const MWWorld::CellStore*, CellMeshes> mCellMeshes;
        std::unordered_map<const MWWorld::CellStore*, CellTerrain> mCellTerrain;
    };
}

#endif // OPENMW_USE_VULKAN
#endif
