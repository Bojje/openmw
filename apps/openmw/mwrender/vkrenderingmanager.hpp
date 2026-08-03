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
#include <osg/Vec4f>

// Vk::Geometry is held by value in CellTerrain, so unlike the other Vk types it cannot be forward
// declared here.
#include <components/vk/vkgeometry.hpp>
#include <components/vk/vkmath.hpp>

#include "vklightcollector.hpp"

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
    class ConstPtr;
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
        ///
        /// Everything the lighting needs that comes from the world rather than the renderer. All the
        /// colours are read back from what the OSG renderer actually uses, so they already carry the
        /// cell's mood colour, the interior brightness floor, and the weather and time of day. They are
        /// gamma-space, as the whole OSG lighting path is, and are decoded to linear on the way in.
        /// The distances are world units and must not be decoded.
        struct FrameLighting
        {
            osg::Vec3f sunLightDir; ///< direction the light travels, i.e. -getSunLightPosition()
            osg::Vec4f sunDiffuse;
            osg::Vec4f ambient;
            osg::Vec4f skyColour;
            osg::Vec4f fogColour;
            float fogStart = 0.0f;
            float fogEnd = 0.0f;
            /// Point lights affecting the visible scene, already collected and gamma-decoded. Empty
            /// when the light manager is unavailable.
            const std::vector<VkPointLight>* pointLights = nullptr;
        };

        void render(Camera& camera, const FrameLighting& lighting);
        bool loadShaders(const std::filesystem::path& shaderDir);

        void addCell(const MWWorld::CellStore* store);
        void removeCell(const MWWorld::CellStore* store);

        // Brings the loaded cell set in line with the OSG side's active cells. Called once per frame so
        // the Vulkan renderer stays a passive observer of the world rather than requiring hooks in
        // MWWorld::Scene. The type matches MWWorld::Scene::CellStoreCollection.
        void syncCells(const std::set<MWWorld::CellStore*, std::less<>>& activeCells);

        void resize(uint32_t width, uint32_t height);

        /// The backend itself, for the parts of the engine that draw through it rather than through
        /// this manager -- the user interface being the one that exists. Never null once constructed.
        Vk::Renderer& renderer() { return *mRenderer; }

        /// Asks for the next rendered frame to be copied out. Nothing is written until
        /// writeScreenshot is called after that frame -- the copy has to be recorded inside the
        /// frame, because a presented swapchain image is no longer the application's to touch. See
        /// Vk::Renderer::requestScreenshot.
        void requestScreenshot();

        /// Writes the frame requested above into \a screenshotPath as \a format, and returns the
        /// file it wrote, or an empty path if no frame was captured. Call once per frame after
        /// render(); it is a no-op unless a screenshot was asked for.
        ///
        /// The OSG side gets its screenshot from osgViewer, which reads the GL framebuffer. This is
        /// the equivalent for the Vulkan side and it exists for the same reason plus one more:
        /// grabbing the screen is not a dependable way to see this renderer's output, because a
        /// window the compositor has put on an overlay plane captures as solid black.
        std::filesystem::path writeScreenshot(
            const std::filesystem::path& screenshotPath, const std::string& format);

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
                // Whether this chunk is offered to the acceleration structure. False for the water
                // surface: it is opaque in the raster pass, so putting it in the TLAS would have it
                // block the sun for everything beneath it and turn every seabed black.
                bool inTlas = true;
            };
            std::vector<Chunk> chunks;
            // Cell-local vertices plus one translation to the cell origin, so terrain coordinates stay
            // small instead of running out to the ±250,000 units the world spans.
            float transform[16];
        };

        // Instances whose transform changes from frame to frame -- actors. Kept apart from
        // mCellMeshes because that is baked once at cell load, which is exactly right for a rock and
        // exactly wrong for something that walks. Rebuilt in full every frame from the active cells;
        // there are a handful of actors in a loaded grid, so rebuilding costs less than tracking
        // which ones moved, and it cannot go stale.
        struct ActorInstance
        {
            size_t meshIndex;
            float transform[16];
        };
        std::vector<ActorInstance> mActorInstances;

        // Rebuilds mActorInstances. Called from syncCells, which the engine calls once per frame.
        void syncActors(const std::set<MWWorld::CellStore*, std::less<>>& activeCells);

        // Appends every mesh of one actor at its current position. Shared by the cell walk and by
        // the player, who is in no cell's reference list and has to be added by hand.
        void addActorInstances(const MWWorld::ConstPtr& ptr);

        // A single NIF yields several submeshes, so the cache maps a model path to all of the
        // mMeshes indices it produced. Returns nullptr only if the model could not be loaded.
        const std::vector<size_t>* getOrLoadMeshes(const std::string& model);

        // Builds and uploads the heightfield for an exterior cell. Does nothing for interiors or for
        // exteriors with no LAND record.
        void addTerrain(const MWWorld::CellStore* store);

        // Appends a flat water surface across the whole cell at its water height. A first pass and
        // honest about it: one opaque quad with the first frame of the vanilla water texture on it,
        // no animation, no transparency, no refraction and no reflection. Morrowind's water is very
        // nearly opaque seen from above, so this reads as water at a distance and as a hard sheet up
        // close, which is a different thing from the Bitter Coast having no water in it at all.
        void addWater(const MWWorld::CellStore* store, CellTerrain& terrain);

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

        // Rebuilds the renderer's sampler array from the textures the currently loaded cells actually
        // reference, and frees the ones none of them do.
        //
        // The array cannot simply mirror mTextures. mTextures only ever grew, and the sampler array
        // is capped at Vk::maxSceneTextures, so a long enough walk silently pushed later textures
        // past the end of it -- where they resolve to the white fallback. That is worse than a
        // missing texture: alpha-tested foliage sampling white alpha stops being cut out, so every
        // leaf billboard goes back to casting a solid rectangular shadow and undoes trap 13.
        //
        // Bounding it by *live* content instead of by history is what makes the cap unreachable: a
        // 3x3 exterior grid references a couple of hundred textures no matter how far the player has
        // walked.
        void syncTexturesToRenderer();

        // Every texture index the loaded cells reference, via their instances' meshes and their
        // terrain chunks. Recomputed whenever the cell set changes rather than tracked incrementally,
        // because the cell set is tiny and a refcount that drifts would fail silently and rarely.
        std::vector<bool> collectLiveTextures() const;

        // "This mesh or terrain chunk has no usable texture." Lives in the header rather than the
        // .cpp because textureSlot below is inline and has to see it.
        static constexpr size_t sNoTexture = static_cast<size_t>(-1);

        // Storage index in mTextures to sampler array slot. Slot 0 is the white fallback, which is
        // also where the sNoTexture sentinel and any evicted texture land.
        uint32_t textureSlot(size_t textureIndex) const
        {
            if (textureIndex == sNoTexture || textureIndex >= mTextureSlots.size())
                return 0u;
            return mTextureSlots[textureIndex];
        }

        std::unordered_map<std::string, size_t> mTextureCache;
        std::vector<std::unique_ptr<Vk::Texture>> mTextures;
        // Parallel to mTextures. An evicted slot keeps its name so getOrLoadTexture can reload into
        // the same index, which is what lets mMeshTextures and the terrain chunks keep holding plain
        // indices across an eviction instead of needing to be rewritten.
        std::vector<std::string> mTextureNames;
        // Maps an index in mTextures to its slot in the renderer's sampler array, or 0 -- the white
        // fallback -- when the texture is not currently live. Meshes are submitted with the slot, not
        // the storage index; the two were the same thing before eviction existed and conflating them
        // again is the way this breaks.
        std::vector<uint32_t> mTextureSlots;
        // Exactly what was last handed to the renderer, so a sync that changes nothing can return
        // without a device idle. Compared by value rather than by length: the live set can change
        // without changing size when one cell's texture replaces another's.
        std::vector<VkImageView> mUploadedTextureViews;

        // How many textures may stay resident before unreferenced ones are actually freed. Eviction
        // is deliberately lazy: the cell grid churns as the player walks and a texture dropped on one
        // crossing is usually wanted on the next, so freeing eagerly trades VRAM for repeated
        // blocking staging uploads on the load path. Well above what a 3x3 exterior grid needs
        // (~230), so in practice this only fires after a long walk across varied regions.
        static constexpr size_t sTextureResidencyLimit = 1024;

        std::unordered_map<const MWWorld::CellStore*, CellMeshes> mCellMeshes;
        std::unordered_map<const MWWorld::CellStore*, CellTerrain> mCellTerrain;

        // Previous frame's view matrix. Composed with this frame's viewInverse into the view-to-view
        // transform the shaders reproject through; deliberately not a view-projection, see render().
        Vk::Mat4 mPrevView;
        uint32_t mFrameIndex = 0;
        bool mLoggedCullRatio = false;
    };
}

#endif // OPENMW_USE_VULKAN
#endif
