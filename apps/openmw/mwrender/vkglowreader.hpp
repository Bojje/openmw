#ifndef OPENMW_MWRENDER_VKGLOWREADER_H
#define OPENMW_MWRENDER_VKGLOWREADER_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace osg
{
    class Node;
}

namespace MWRender
{
    /// Reads the live enchanted-item glow off an object in the OSG scene graph.
    ///
    /// This renderer does not decide what glows and does not run the glow's animation. OSG owns
    /// both: MWRender::ObjectAnimation calls SceneUtil::addEnchantedGlow for any reference whose
    /// class reports an enchantment, and SceneUtil::GlowUpdater then swaps one of 32 caustic
    /// textures onto the object root every update traversal. Asking the ESM store whether a
    /// reference is enchanted, and recomputing which caustic frame is due, would be a second
    /// opinion about the same world -- and the second opinion is the one that goes wrong on a
    /// paused game, a changed timescale, or a spell-cast glow that OSG added and this side never
    /// heard about.
    ///
    /// So there is nothing here but a read. The colour and the current frame are taken off the
    /// stateset OSG installed on the object root this frame, and the frame is named by its file
    /// name so it resolves through the caller's ordinary texture loader.
    ///
    /// Nothing here is verified against a running game -- it has not been built or run.
    class GlowReader
    {
    public:
        /// \a resolveTexture maps an image file name to its storage index in the caller's texture
        /// table, loading it if needed, so the caustic frames are cached, evicted and shared on the
        /// same terms as everything else.
        ///
        /// A storage index and not a sampler slot. The slot is not settled until the caller's
        /// texture sync has run, and that sync renumbers the whole array rather than appending to
        /// it, so a slot resolved here names a different texture on any frame a cell unloads. The
        /// caller turns the index into a slot when it submits the instance, which is where every
        /// mesh's texture is resolved too.
        explicit GlowReader(std::function<std::size_t(const std::string&)> resolveTexture);

        /// Call once at the start of each sweep, before any read().
        void beginFrame();

        /// Whether \a objectBase is glowing right now, and with what.
        ///
        /// \a objectBase is the reference's base node -- the PositionAttitudeTransform that
        /// RefData hands out. The glow lives on the object root below it, because that is the node
        /// addEnchantedGlow was given.
        ///
        /// Returns false for anything not glowing, which is the answer for all but a handful of
        /// references in the game, and it returns false cheaply: the common case costs one node
        /// mask test and one stateset pointer.
        ///
        /// \a outTextureIndex receives the caustic frame's storage index, not its sampler slot, and
        /// is left untouched when this returns false. The refusal to draw a glow on the white
        /// fallback still exists but is made at the far end, where the caller resolves the index:
        /// there is no slot to test here, because the texture sync that assigns slots runs later in
        /// the frame than this does.
        bool read(const osg::Node& objectBase, float outColour[3], std::size_t& outTextureIndex);

        /// Storage indices of every caustic frame this reader has resolved, or nothing at all when
        /// nothing in the loaded cells is enchanted this frame.
        ///
        /// All 32 frames, not the one currently bound, and that is the point. A caustic frame is
        /// bound for 1/16 s and comes round again two seconds later; reported one at a time, the
        /// other 31 look dead, lose their sampler slots and resolve to the white fallback -- and a
        /// white fallback here is not a subtle error, it is a solid white object-shaped blob added
        /// at full strength over the scene. The 60-frame residency grace period is not enough on
        /// its own: two seconds is 120 frames at 60 Hz.
        const std::vector<std::size_t>& textureIndices() const;

    private:
        std::function<std::size_t(const std::string&)> mResolveTexture;
        // Every caustic storage index seen since startup. Never cleared: the set is at most 32
        // entries and they are wanted together or not at all.
        std::vector<std::size_t> mCausticIndices;
        // Returned empty when nothing in the cell is enchanted, so that walking out of the one room
        // in the game with an enchanted dagger on the floor gives 32 sampler slots back.
        std::vector<std::size_t> mEmpty;
        // "A caustic stateset was found this frame", not "a glow was drawn this frame". read() used
        // to set it on the second reading, which could never become true: the frames this flag gates
        // are the frames that have to be live before any of them can resolve to a slot at all.
        bool mAnyGlowThisFrame = false;
    };
}

#endif

#endif
