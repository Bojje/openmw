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
        /// \a resolveTexture maps an image file name to a slot in the renderer's sampler array. It
        /// is the caller's texture loader, so the caustic frames are cached, evicted and shared on
        /// the same terms as everything else. It returns the sampler slot to draw with and, through
        /// its out parameter, the storage index that slot came from -- the caller needs the second
        /// to keep the texture alive across eviction.
        explicit GlowReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture);

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
        bool read(const osg::Node& objectBase, float outColour[3], uint32_t& outSlot);

        /// Storage indices of every caustic frame this reader has resolved, or nothing at all when
        /// no object glowed this frame.
        ///
        /// All 32 frames, not the one currently bound, and that is the point. A caustic frame is
        /// bound for 1/16 s and comes round again two seconds later; reported one at a time, the
        /// other 31 look dead, lose their sampler slots and resolve to the white fallback -- and a
        /// white fallback here is not a subtle error, it is a solid white object-shaped blob added
        /// at full strength over the scene. The 60-frame residency grace period is not enough on
        /// its own: two seconds is 120 frames at 60 Hz.
        const std::vector<std::size_t>& textureIndices() const;

    private:
        std::function<uint32_t(const std::string&, std::size_t&)> mResolveTexture;
        // Every caustic storage index seen since startup. Never cleared: the set is at most 32
        // entries and they are wanted together or not at all.
        std::vector<std::size_t> mCausticIndices;
        // Returned empty when nothing glowed, so that walking out of the one room in the game with
        // an enchanted dagger on the floor gives 32 sampler slots back.
        std::vector<std::size_t> mEmpty;
        bool mAnyGlowThisFrame = false;
    };
}

#endif

#endif
