#ifndef OPENMW_MWRENDER_VKSKYREADER_H
#define OPENMW_MWRENDER_VKSKYREADER_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <components/vk/vkrenderer.hpp>

#include <osg/Vec3f>

namespace osg
{
    class Node;
}

namespace MWRender
{
    class SkyMeshCache;

    /// Collects the sun disc, the two moons, the cloud layer and the night sky out of the live OSG sky
    /// graph as something the Vulkan renderer can draw.
    ///
    /// Read rather than recomputed, for the same reason ParticleReader reads osgParticle instead of
    /// simulating. The sky is a simulation too: WeatherManager drives the sun's arc, the moons' phase,
    /// their rise and set, the transparency that fades them out at dawn and the colour a Blight storm
    /// tints them, and all of it lands on the sky graph every frame through the update traversal --
    /// which runs whichever backend is presenting. Recomputing any of it here would make this the
    /// first place the two backends could disagree about the same sky, and would throw away the only
    /// check there is: putting the two images side by side.
    ///
    /// The moon phase is the sharpest example. There are eight phase images per moon and the choice
    /// between them is Morrowind's calendar arithmetic. Moon::setPhase has already done that
    /// arithmetic and has already put the answer on the stateset, as the file name of the texture on
    /// unit 0. Reading that name is a lookup; redoing the arithmetic is a second implementation of the
    /// lunar calendar that can be a day out.
    ///
    /// The clouds and the stars are the same argument again with more moving parts. The cloud layer is
    /// two copies of one mesh crossfading between two weathers, each independently rotated to its own
    /// storm direction, scrolling on a timer that optionally runs on the game clock rather than the
    /// wall clock, tinted by the fog colour of the moment. Every one of those numbers is already
    /// public on the graph -- the crossfade as the `opacity` uniform, the rotation as the transform's
    /// attitude, the scroll as a texture matrix, the tint as a material emission -- and every one of
    /// them is a thing WeatherManager can change at any time. The star field is a mesh rolled 360
    /// degrees per four days by the same calendar the moons use.
    ///
    /// The atmosphere dome, pass 0, is deliberately NOT collected. It is a flat emission colour with a
    /// vertical alpha ramp over a clear colour set to the fog colour, and files/shaders/vulkan/
    /// composite.frag already paints exactly that as a lerp along the view ray -- so drawing the dome
    /// as well would composite the sky colour on top of itself and lighten the whole upper sky.
    ///
    /// Nothing here is verified against a running game -- it has not been built or run. What is
    /// verified is that it matches the graph the OSG backend builds, read out of skyutil.cpp and
    /// sky.cpp; every place where that reading is load-bearing carries a line reference.
    class SkyReader
    {
    public:
        /// \a resolveTexture maps an image file name to its storage index in the caller's texture
        /// table, loading it if needed, so the sun, moon, cloud and star images are cached, evicted
        /// and shared on the same terms as everything else.
        ///
        /// A storage index and not a sampler slot. The slot is not settled until the caller's
        /// texture sync has run, which is later in the frame than this reader runs, and that sync
        /// renumbers the array rather than appending to it -- so a slot resolved here names a
        /// different texture on every frame a cell unloads, and what that looks like in the sky is a
        /// moon drawn with a rock face. See resolveTextureSlots.
        ///
        /// \a device and \a commandPool are only ever used to upload the two sky meshes, once each, on
        /// the first frame the sky is visible. They are taken by reference because the renderer that
        /// owns them outlives this: SkyReader is declared after it in VkRenderingManager and is
        /// therefore destroyed first.
        SkyReader(std::function<std::size_t(const std::string&)> resolveTexture,
            Vk::Device& device, Vk::CommandPool& commandPool);
        ~SkyReader();

        /// Walks \a sceneRoot and refills the element and mesh lists. The walk stops at any node the
        /// sky has hidden by clearing its mask, and the per-node work below the sky is skipped
        /// entirely for anything that is not under it, so this is cheap enough to do every frame.
        void collect(osg::Node* sceneRoot);

        /// Turns the storage indices collect() recorded into the sampler slots \a slotOf answers
        /// now, and applies the two refusals that cannot be made until a slot is known. Call once
        /// per collect(), after the caller's texture sync and before anything below is read.
        ///
        /// Once: a second pass would read a slot as though it were a storage index. Nothing this
        /// class hands out is usable before it runs -- elements(), sunFlash(), meshes() and
        /// sunDiscTexture() all carry indices until then.
        ///
        /// The two refusals used to live in the collector, where they tested a slot resolved at read
        /// time. That is the number this whole change exists to stop trusting, so they moved here,
        /// which is the earliest point the answer is real. Both are about slot 0, the 1x1 white
        /// fallback, and both matter:
        ///
        ///  - the flash is drawn thirty degrees across with the depth test off, so a white fallback
        ///    there is a white sheet over most of the screen. The flash is dropped for the frame.
        ///  - the visibility disc is alpha tested at 0.8 to reproduce PASS_SUNFLASH_QUERY, and the
        ///    fallback's alpha is 1 everywhere, so all 64 rays would pass and the sun would measure
        ///    as its whole quad rather than its bright core. The disc is left invalid, which sets
        ///    the ray count to zero, which sky.vert reads as fully visible.
        ///
        /// Dropping the disc drops the flash with it. The old read-time version did that as a side
        /// effect of sharing one `valid` flag; it is kept on purpose, because with no disc the
        /// shader is told the sun is fully visible, so a flash drawn on that frame would be drawn at
        /// full size on the one frame nothing measured the sun.
        void resolveTextureSlots(const std::function<uint32_t(std::size_t)>& slotOf);

        /// The billboards -- the sun disc and the two moons. Emitted in graph order, which is the
        /// order OSG's sky render bin draws them in. Sampler slots only after resolveTextureSlots.
        const std::vector<Vk::SkyElement>& elements() const { return mElements; }

        /// The sun flash, or null when the sun is not in the sky.
        ///
        /// Kept out of elements() deliberately. Everything in that list is background -- it is
        /// drawn early, only where the depth buffer is still cleared, and can never cover
        /// geometry. The flash is the opposite: upstream draws it with the depth test off in
        /// RenderBin_SunGlare, over the world and over the water, which is what keeps it a whole
        /// circle as the sun sinks behind a ridge. Put it in the same list and it would be drawn
        /// in the same place, and it would vanish the moment anything was in front of the sun.
        const Vk::SkyElement* sunFlash() const { return mHasSunFlash ? &mSunFlash : nullptr; }

        /// Unit direction from the camera to the centre of the sun disc, in world space, and the
        /// tangent of the disc's angular radius. Both zero when there is no sun.
        ///
        /// Taken from the disc billboard rather than from CelestialBody's 450 and 1000, so a mod
        /// that resizes the sun moves the visibility test with it and nothing here is told.
        const osg::Vec3f& sunDiscDirection() const { return mSunDiscDirection; }
        float sunDiscTanRadius() const { return mSunDiscTanRadius; }

        /// Sampler slot of the sun disc texture, for the 0.8 alpha test the OSG occlusion query
        /// runs through PASS_SUNFLASH_QUERY. Zero when there is no sun, and zero until
        /// resolveTextureSlots has run -- which is also the answer for a frame in which the disc
        /// texture has no slot, because that is the case resolveTextureSlots clears the whole disc
        /// for.
        uint32_t sunDiscTexture() const { return mSunDiscTexture; }

        /// The sun's material diffuse alpha, which is SunGlareCallback's mGlareView.
        ///
        /// The same number twice: Sun::adjustTransparency writes the ratio into the updater's
        /// colour alpha and hands it to both callbacks in the same breath (skyutil.cpp lines
        /// 713-720), so reading the material off the graph recovers exactly what the callbacks
        /// were given. One is public and the other is a private member of a cull callback.
        float sunGlareView() const { return mSunGlareView; }

        /// The cloud layer and the night sky, as draws against buffers this owns. Also in graph order;
        /// each carries the flag that says whether it belongs in front of the sun and moons or behind
        /// them, because the two lists are drawn separately and their relative order is lost.
        const std::vector<Vk::SkyMeshDraw>& meshes() const { return mMeshes; }

        /// Storage indices of every texture referenced this frame, for the caller's live set. Without
        /// this the eviction pass sees the sun, moon, cloud and star textures referenced by no mesh, no
        /// actor and no terrain chunk, decides they are dead, and replaces them with the white fallback
        /// -- so the sun and both moons draw as plain white squares and the cloud layer becomes a solid
        /// white sheet over the entire sky. This is the same trap ParticleReader documents, and it
        /// bites here harder: a white square in the sky is the most visible thing on screen.
        const std::vector<std::size_t>& textureIndices() const { return mTextureIndices; }

    private:
        std::function<std::size_t(const std::string&)> mResolveTexture;
        // Owns the uploaded sky geometry, so it has to outlive every mMeshes entry pointing into it --
        // which it does, both being members here and this one declared first.
        std::unique_ptr<SkyMeshCache> mMeshCache;
        std::vector<Vk::SkyElement> mElements;
        std::vector<Vk::SkyMeshDraw> mMeshes;
        std::vector<std::size_t> mTextureIndices;
        // params[0] and params[1] hold storage indices between collect() and resolveTextureSlots,
        // the same as the elements and the meshes do. In the GPU fields rather than in parallel
        // arrays because that is what the particle quads have to do and there is no reason for the
        // two readers to differ; the lists are never reordered here, so the only thing to keep
        // straight is that the resolve runs once.
        Vk::SkyElement mSunFlash = {};
        bool mHasSunFlash = false;
        osg::Vec3f mSunDiscDirection;
        float mSunDiscTanRadius = 0.f;
        // The disc keeps both. The index is what survives a renumbering and what the refusal is
        // decided from; the slot is what raygen.rgen indexes the sampler array with. Separate
        // members rather than one field changing meaning, because this is the one the two slot-0
        // refusals hang off and it is worth being able to read it twice and get the same answer.
        std::size_t mSunDiscTextureIndex = static_cast<std::size_t>(-1);
        uint32_t mSunDiscTexture = 0;
        float mSunGlareView = 0.f;
    };
}

#endif
#endif
