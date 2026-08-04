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
        /// \a resolveTexture maps an image file name to a slot in the renderer's sampler array. It is
        /// the caller's texture loader, so the sun, moon, cloud and star images are cached, evicted and
        /// shared on the same terms as everything else.
        ///  resolveTexture returns the sampler slot to draw with and, through its out parameter, the
        /// storage index that slot came from -- the caller needs the second to keep the texture alive
        /// across eviction.
        ///
        /// \a device and \a commandPool are only ever used to upload the two sky meshes, once each, on
        /// the first frame the sky is visible. They are taken by reference because the renderer that
        /// owns them outlives this: SkyReader is declared after it in VkRenderingManager and is
        /// therefore destroyed first.
        SkyReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture,
            Vk::Device& device, Vk::CommandPool& commandPool);
        ~SkyReader();

        /// Walks \a sceneRoot and refills the element and mesh lists. The walk stops at any node the
        /// sky has hidden by clearing its mask, and the per-node work below the sky is skipped
        /// entirely for anything that is not under it, so this is cheap enough to do every frame.
        void collect(osg::Node* sceneRoot);

        /// The billboards -- the sun disc and the two moons. Emitted in graph order, which is the
        /// order OSG's sky render bin draws them in.
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
        /// runs through PASS_SUNFLASH_QUERY. Zero when there is no sun.
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
        std::function<uint32_t(const std::string&, std::size_t&)> mResolveTexture;
        // Owns the uploaded sky geometry, so it has to outlive every mMeshes entry pointing into it --
        // which it does, both being members here and this one declared first.
        std::unique_ptr<SkyMeshCache> mMeshCache;
        std::vector<Vk::SkyElement> mElements;
        std::vector<Vk::SkyMeshDraw> mMeshes;
        std::vector<std::size_t> mTextureIndices;
        Vk::SkyElement mSunFlash = {};
        bool mHasSunFlash = false;
        osg::Vec3f mSunDiscDirection;
        float mSunDiscTanRadius = 0.f;
        uint32_t mSunDiscTexture = 0;
        float mSunGlareView = 0.f;
    };
}

#endif
#endif
