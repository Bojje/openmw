#ifndef OPENMW_MWRENDER_VKRENDERINGMANAGER_H
#define OPENMW_MWRENDER_VKRENDERINGMANAGER_H

#ifdef OPENMW_USE_VULKAN

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <array>
#include <unordered_map>
#include <vector>

#include <osg/Quat>
#include <osg/Vec3f>
#include <osg/Vec4f>
#include <osg/ref_ptr>

#include <components/sceneutil/positionattitudetransform.hpp>

namespace osg
{
    class Node;
}

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
    class Ptr;
}

namespace MWRender
{
    class Camera;
    class LandComposite;
    class ParticleReader;
    class SkyReader;
    class GlowReader;
    struct LandBlend;

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
            /// Whether the player is in an interior cell. Changes what \a ambient means and therefore
            /// how it is occluded -- see SceneData::isInterior.
            bool isInterior = false;
        };

        void render(Camera& camera, const FrameLighting& lighting);

        /// The OSG scene graph, which this renderer reads but never owns. Needed because particle
        /// effects are simulated by osgParticle and read from there every frame -- see ParticleReader
        /// for why that is the arrangement rather than simulating them here.
        void setSceneRoot(osg::Node* sceneRoot) { mSceneRoot = sceneRoot; }
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

                // The enchanted glow this instance draws with, refreshed every frame by
                // refreshMovedObjects. A glowTexture of 0 -- the white fallback slot -- means it
                // does not glow, which is the case for all but a few hundred references in the
                // game.
                //
                // Per instance rather than per mesh, because the glow belongs to the *reference*
                // and not to the model: two iron daggers on the same table share a mesh and only
                // one of them is enchanted.
                float glowColour[3] = { 0.0f, 0.0f, 0.0f };
                uint32_t glowTexture = 0;

                // Whether this instance's BLAS goes into the acceleration structure.
                //
                // Cleared for good the first time the object it belongs to is seen to move, which is
                // the same treatment actors already get and for the same two reasons: buildTlas idles
                // the device and rebuilds the whole structure, so a thing that moves every frame would
                // stall the device every frame, and the denoiser's reprojection is exact only because
                // every instance in the TLAS is static world geometry (Vk::SceneData::prevViewFromCurView).
                //
                // The cost is that a door which has been opened once casts no ray traced shadow and
                // appears in no reflection for the rest of the cell's life, even after it comes to
                // rest. Putting it back would cost a second device idle, and doors get opened and shut
                // again, so it would be a stall per swing rather than one per door.
                bool traced = true;
            };
            std::vector<Instance> instances;

            // The objects in this cell whose placement is worth re-reading, and where their instances
            // live in the vector above.
            //
            // One entry per *object*, not per instance: a NIF yields several submeshes and they all
            // move together, so the compare that decides whether anything moved happens once and the
            // matrix multiply that follows happens per submesh only when it did.
            //
            // Not every object gets an entry. See trackableNode in the .cpp for the three cases that
            // are deliberately left out, of which object paging's shared sentinel node is the one that
            // would do visible damage.
            struct MovedObject
            {
                // Held by ref_ptr rather than as a raw pointer, which is not defensive coding: taking
                // an item out of a container or disabling a reference clears RefData's node, and the
                // object we are pointing at would be freed underneath us mid-cell while our instances
                // are still being submitted every frame. A held node that has left the graph simply
                // stops changing, which is the same thing as not moving.
                osg::ref_ptr<const SceneUtil::PositionAttitudeTransform> node;

                // What the node said last time we looked. Copies of OSG's own floats, so an object
                // that has not been written to compares bit-identical.
                osg::Vec3f position;
                osg::Quat attitude;
                osg::Vec3f scale;

                uint32_t firstInstance = 0;
                uint32_t instanceCount = 0;
                // Whether this object has already left the acceleration structure. Latched, so the
                // TLAS is rebuilt once per object rather than once per frame of its swing.
                bool detached = false;
            };
            std::vector<MovedObject> tracked;
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

        // Instances whose transform changes from frame to frame -- actors. Kept apart from
        // mCellMeshes because that is baked once at cell load, which is exactly right for a rock and
        // exactly wrong for something that walks. Rebuilt in full every frame from the active cells;
        // there are a handful of actors in a loaded grid, so rebuilding costs less than tracking
        // which ones moved, and it cannot go stale.
        struct ActorInstance
        {
            size_t meshIndex;
            float transform[16];
            // Where this instance's bone palette starts in mSkinMatrices, or Vk::sNoBones if it has
            // none and is therefore drawn rigidly. Per instance and not per mesh: two NPCs share the
            // same shirt mesh and stand in different poses.
            uint32_t boneOffset = Vk::sNoBones;

            // World-space bounds of a skinned instance, valid only when boneOffset is set.
            //
            // A skinned shape cannot be culled on its mesh's bounds put through its instance
            // transform, because those describe the vertices before the pose and the pose is what
            // decides where they land -- see HANDOFF trap 37, which is what happens when it is tried.
            // These are the union of the mesh's bounds through every one of the instance's bone
            // matrices, which over-estimates -- a vertex is only moved by the bones it is weighted
            // to -- but never under-estimates, and an over-estimate only costs a draw.
            float worldMin[3] = { 0.0f, 0.0f, 0.0f };
            float worldMax[3] = { 0.0f, 0.0f, 0.0f };
        };
        std::vector<ActorInstance> mActorInstances;

        // Every bone palette in the frame, laid end to end, four-by-four column major. Rebuilt with
        // mActorInstances and handed to the renderer whole, which is why an instance can name its own
        // palette with a single offset and its vertices can carry eight-bit bone indices.
        std::vector<float> mSkinMatrices;

        // Rebuilds mActorInstances. Called from syncCells, which the engine calls once per frame.
        void syncActors(const std::set<MWWorld::CellStore*, std::less<>>& activeCells);

        // Re-reads the placement of every tracked object and rewrites the instance transforms of the
        // ones that moved. Returns true if any object left the acceleration structure this frame, in
        // which case the caller must mark the TLAS dirty.
        //
        // This is the fix for doors that never swing and levers that never throw. mCellMeshes is baked
        // once at cell load out of the reference's ESM::Position, which is right for a rock and wrong
        // for anything a script or a door state touches afterwards.
        //
        // Deliberately not a scene graph traversal. It walks a flat vector of node pointers and
        // compares ten floats each -- no NodeVisitor, no dynamic_cast, no allocation, and nothing
        // proportional to the size of the graph. A naive version that accepted() the loaded cell root
        // every frame would visit tens of thousands of nodes on a Bitter Coast exterior to find the
        // two that had moved.
        bool refreshMovedObjects();

        // Appends every mesh of one actor at its current position. Shared by the cell walk and by
        // the player, who is in no cell's reference list and has to be added by hand.
        void addActorInstances(const MWWorld::Ptr& ptr);

        // The NPC case, which is not one model but a skeleton plus a dozen body-part files hung on
        // its bones by name, plus whatever they are wearing. Falls back to nothing if the race's
        // parts cannot be resolved.
        void addNpcInstances(const MWWorld::Ptr& ptr, const float objectTransform[16]);

        // Bind pose of a skeleton NIF, by node name, loaded once and cached. Keyed on the skeleton's
        // model path, since beast races use a different one.
        const std::unordered_map<std::string, std::array<float, 16>>* getOrLoadSkeleton(const std::string& model);
        std::unordered_map<std::string, std::unordered_map<std::string, std::array<float, 16>>> mSkeletonCache;

        // A single NIF yields several submeshes, so the cache maps a model path to all of the
        // mMeshes indices it produced. Returns nullptr only if the model could not be loaded.
        const std::vector<size_t>* getOrLoadMeshes(const std::string& model);

        // Builds and uploads the heightfield for an exterior cell. Does nothing for interiors or for
        // exteriors with no LAND record.
        void addTerrain(const MWWorld::CellStore* store);

        // Resolves a raw NIF texture name to an index into mTextures, loading it if needed. Returns
        // npos when the texture cannot be loaded, so callers can fall back to untextured rendering.
        //
        // \a srgb false uploads the pixels as UNORM instead of an _SRGB format. Exactly one caller
        // wants that -- the water normal map, whose texels are directions rather than colours. The
        // cache is keyed on the name alone, so a given name must always be asked for with the same
        // flag or the second caller silently inherits the first one's decoding.
        size_t getOrLoadTexture(const std::string& nifTextureName, bool srgb = true);

        /// Composites a cell's land textures into one and puts it in mTextures, returning its index.
        ///
        /// sNoTexture when there is nothing to composite -- a cell painted with a single texture
        /// needs no blending, and the caller should draw it the old way with that texture directly.
        /// Also sNoTexture if any layer fails to load or the bake itself fails, so a failure here
        /// costs the soft transitions and nothing else.
        ///
        /// The composite goes into mTextures like any other texture, so it participates in eviction
        /// and in the sampler array on the same terms. It is keyed by cell rather than by file name,
        /// since it has no file.
        size_t bakeLandComposite(const LandBlend& blend);

        std::unique_ptr<Vk::Renderer> mRenderer;
        std::unique_ptr<NifVk::MeshConverter> mMeshConverter;
        // Null when the composite pass could not be built, which is not fatal -- terrain then
        // draws the way it did before, one chunk per land texture with hard edges between them.
        std::unique_ptr<LandComposite> mLandComposite;

        // Not owned. OSG's scene root, read once a frame for live particle state.
        osg::Node* mSceneRoot = nullptr;
        std::unique_ptr<ParticleReader> mParticleReader;
        // Reads the same graph for the sun disc and the two moons. Separate from the particle reader
        // because the two find different things in different ways -- the sky bodies are three named
        // quads discriminated by a uniform, not an unbounded set of particle systems -- and because
        // one of them can be switched off without touching the other.
        std::unique_ptr<SkyReader> mSkyReader;
        // Reads the same graph again, but not by walking it: it is asked about one object at a
        // time, from the tracked list refreshMovedObjects already sweeps. A third full traversal to
        // find the handful of glowing objects in a cell would cost more than the feature is worth.
        std::unique_ptr<GlowReader> mGlowReader;

        std::unordered_map<std::string, std::vector<size_t>> mMeshCache;
        std::vector<std::unique_ptr<NifVk::VulkanMesh>> mMeshes;

        // Parallel to mMeshes: the texture index each mesh draws with, or npos if untextured.
        // Kept here rather than on NifVk::VulkanMesh so the NIF converter stays unaware of the
        // renderer's texture table.
        std::vector<size_t> mMeshTextures;

        // Parallel to mMeshes: where this submesh sits relative to its object's origin, so that a
        // placed instance is objectTransform * this and nothing more.
        //
        // For an ordinary shape it is the converter's mesh.transform, the shape's place in the NIF
        // node hierarchy. For a skinned one it is that shape's heaviest bone, boneRestWorld *
        // skinInvBind, because a skinned shape's vertices are in the skin's bind space and its node
        // transform does not describe where they belong. Every banner and flag in Morrowind is
        // skinned and their bind space is a flat quad in the local XY plane, so using the node
        // transform draws them horizontal.
        //
        // One matrix rather than a branch at each use because addCell and refreshMovedObjects both
        // build instance transforms, and if only one of them knew about skinned statics a banner
        // would stand up when its cell loaded and fall flat the first time it was refreshed.
        std::vector<std::array<float, 16>> mMeshPlacements;

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
        std::vector<bool> collectLiveTextures();

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
        // Parallel to mTextures: the value of mTextureSyncCounter when this texture was last drawn
        // with, or 0 for one that never has been.
        //
        // A texture that is resident but not named this frame loses its sampler slot and answers 0,
        // which is the white fallback. That is right for a cell the player has walked out of and
        // wrong for a flipbook: NifOsg::FlipController binds one frame of an animated texture at a
        // time, so the other frames look dead every frame, and each one draws white for a frame when
        // the controller comes back to it. The water ripples do this thirteen times a second.
        std::vector<uint64_t> mTextureLastUsed;
        uint64_t mTextureSyncCounter = 0;
        // Long enough to cover the whole of any flipbook's cycle, not just the gap between two of its
        // frames. 60 was measured against the water ripples, which are four frames, and it is not
        // enough: the enchanted glow's caustic is 32 frames at 16 fps, so each frame is bound for about
        // four frames out of every 120 and every one of them fell out of the live set before its turn
        // came round again. All 32 reported themselves as failing to load, and none of them was
        // failing to load.
        //
        // 300 is five seconds at 60 fps, comfortably past the 2-second caustic cycle. Slots are the
        // scarce thing rather than VRAM -- the sampler array is 1024 and eviction is a separate, much
        // lazier decision -- so the cost of being generous here is small and the cost of being tight is
        // a texture that reports itself broken while working perfectly.
        static constexpr uint64_t sTextureSlotGraceFrames = 300;
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
        // Tied to the sampler array rather than chosen independently, and that coupling is the fix for
        // a real bug: it was 1024 while the array held 512, so a texture-dense cell went past what the
        // array could address and the excess rendered as the white fallback. Ghostgate's Tower of Dusk
        // needs 709 and came out as blank white walls. Whatever these two are, the second must not
        // exceed the first.
        // Must not exceed Vk::maxSceneTextures - 1, and that is not a style note: it was 1024 while
        // the sampler array held 512, so a texture-dense cell went past what the array could address
        // and the excess rendered as the 1x1 white fallback. Ghostgate's Tower of Dusk needs 709
        // resident and came out as blank white walls beside correctly textured arches.
        //
        // Spelled as a literal because this header does not include vkrenderer.hpp; a static_assert in
        // the .cpp, which does, is what actually holds the two together.
        static constexpr size_t sTextureResidencyLimit = 1023;

        std::unordered_map<const MWWorld::CellStore*, CellMeshes> mCellMeshes;
        std::unordered_map<const MWWorld::CellStore*, CellTerrain> mCellTerrain;

        // The water surface's normal map, textures/omw/water_nm.png, loaded once and then kept alive
        // by collectLiveTextures.
        //
        // It belongs to no mesh, no actor and no terrain chunk -- the water grid is generated in
        // water.vert and exists in no cell -- so without that mark it is dead the instant it loads,
        // loses its slot, and resolves to the 1x1 white fallback. White decodes to a normal of
        // (1, 1, 1), which flattens every wave in the game and tilts what is left the same way.
        size_t mWaterNormalTexture = sNoTexture;
        bool mWaterNormalRequested = false;

        // Seconds since the renderer started, for the wave scroll. Accumulated in double because it
        // never resets: fp32 starts quantising the scroll after a long enough session, and wrapping
        // it instead makes the waves jump, because the six octaves scroll at speeds that never come
        // back to an integer offset together.
        double mWaterSeconds = 0.0;

        // Previous frame's view matrix. Composed with this frame's viewInverse into the view-to-view
        // transform the shaders reproject through; deliberately not a view-projection, see render().
        Vk::Mat4 mPrevView;
        uint32_t mFrameIndex = 0;
        bool mLoggedCullRatio = false;
    };
}

#endif // OPENMW_USE_VULKAN
#endif
