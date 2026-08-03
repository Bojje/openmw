#ifndef OPENMW_MWRENDER_VKSKYREADER_H
#define OPENMW_MWRENDER_VKSKYREADER_H

#ifdef OPENMW_USE_VULKAN

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include <components/vk/vkrenderer.hpp>

namespace osg
{
    class Node;
}

namespace MWRender
{
    /// Collects the sun disc and the two moons out of the live OSG sky graph as billboards the
    /// Vulkan renderer can draw.
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
    /// Nothing here is verified against a running game -- it has not been built or run. What is
    /// verified is that it matches the graph the OSG backend builds, read out of skyutil.cpp and
    /// sky.cpp; every place where that reading is load-bearing carries a line reference.
    class SkyReader
    {
    public:
        /// \a resolveTexture maps an image file name to a slot in the renderer's sampler array. It is
        /// the caller's texture loader, so the sun and moon images are cached, evicted and shared on
        /// the same terms as everything else.
        ///  resolveTexture returns the sampler slot to draw with and, through its out parameter, the
        /// storage index that slot came from -- the caller needs the second to keep the texture alive
        /// across eviction.
        explicit SkyReader(std::function<uint32_t(const std::string&, std::size_t&)> resolveTexture);

        /// Walks \a sceneRoot and refills the element list. There are never more than three of these,
        /// and the walk stops at any node the sky has hidden by clearing its mask, so this is cheap
        /// enough to do every frame.
        void collect(osg::Node* sceneRoot);

        /// Emitted in graph order, which is the order OSG's sky render bin draws them in.
        const std::vector<Vk::SkyElement>& elements() const { return mElements; }

        /// Storage indices of every texture referenced this frame, for the caller's live set. Without
        /// this the eviction pass sees the sun and moon textures referenced by no mesh, no actor and
        /// no terrain chunk, decides they are dead, and replaces them with the white fallback -- so
        /// the sun and both moons draw as plain white squares. This is the same trap ParticleReader
        /// documents, and it bites here harder: a white square in the sky is the most visible thing
        /// on screen.
        const std::vector<std::size_t>& textureIndices() const { return mTextureIndices; }

    private:
        std::function<uint32_t(const std::string&, std::size_t&)> mResolveTexture;
        std::vector<Vk::SkyElement> mElements;
        std::vector<std::size_t> mTextureIndices;
    };
}

#endif
#endif
