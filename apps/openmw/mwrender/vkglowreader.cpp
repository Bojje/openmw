#ifdef OPENMW_USE_VULKAN

#include "vkglowreader.hpp"

#include <algorithm>

#include <osg/Group>
#include <osg/Image>
#include <osg/Node>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Vec4f>

namespace
{
    /// How far below the reference's base node to look for the glow.
    ///
    /// Two is enough and more would be waste. SceneUtil::addEnchantedGlow is given
    /// Animation::mObjectRoot, and setObjectRoot puts that either directly under the base node or
    /// under one inserted transform, so the glow stateset is at depth 0, 1 or 2 and never deeper.
    /// Descending the whole object instead would walk every shape in every crate and barrel in the
    /// cell, every frame, to discover every frame that none of them is enchanted -- which is the
    /// entire cost of this feature, spent on the 99.85% of references that do not glow.
    constexpr int sMaxGlowDepth = 2;

    /// Whether \a name is one of the 32 caustic frames the enchanted glow cycles through.
    ///
    /// The file name is the marker rather than SceneUtil::TextureType("envMap"), which would look
    /// like the more principled test and is not: NiTextureEffect sphere maps carry that same type
    /// string (nifloader.cpp line 656) and so does every NIF that reflects its surroundings, so
    /// keying on it would put a magic-item glow on glass armour and Dwemer metal. addEnchantedGlow
    /// only ever binds textures/magicitem/caustNN.dds, and nothing else in the game does.
    bool isCausticFrame(const std::string& name)
    {
        return name.find("magicitem") != std::string::npos && name.find("caust") != std::string::npos;
    }

    /// The glow stateset at or below \a node, or null.
    const osg::StateSet* findGlowStateSet(const osg::Node& node, int depth)
    {
        // A cleared node mask is how OpenMW hides things, and it is honoured here for the same
        // reason the particle and sky readers honour it. A holstered weapon that has been drawn has
        // its sheath node masked off (actoranimation.cpp line 385) while keeping its stateset, so
        // ignoring the mask would leave a sword-shaped glow hanging at the hip of every NPC who had
        // drawn one.
        if (node.getNodeMask() == 0)
            return nullptr;

        if (const osg::StateSet* stateSet = node.getStateSet())
        {
            // Read off the node's *current* stateset each time rather than remembering the pointer.
            // SceneUtil::StateSetUpdater double-buffers two shallow copies and installs whichever
            // belongs to this traversal (statesetupdater.cpp line 37), so the pointer alternates
            // between two objects every frame; a cached one is right half the time and one frame
            // stale the other half, which shows up as the glow flickering at half the frame rate.
            const osg::StateSet::TextureAttributeList& units = stateSet->getTextureAttributeList();
            for (unsigned int unit = 0; unit < units.size(); ++unit)
            {
                const auto* texture = dynamic_cast<const osg::Texture2D*>(
                    stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                if (texture == nullptr || texture->getImage() == nullptr)
                    continue;
                if (isCausticFrame(texture->getImage()->getFileName()))
                    return stateSet;
            }
        }

        if (depth >= sMaxGlowDepth)
            return nullptr;

        if (const osg::Group* group = node.asGroup())
        {
            for (unsigned int i = 0; i < group->getNumChildren(); ++i)
            {
                if (const osg::StateSet* found = findGlowStateSet(*group->getChild(i), depth + 1))
                    return found;
            }
        }

        return nullptr;
    }
}

namespace MWRender
{
    GlowReader::GlowReader(std::function<std::size_t(const std::string&)> resolveTexture)
        : mResolveTexture(std::move(resolveTexture))
    {
    }

    void GlowReader::beginFrame()
    {
        mAnyGlowThisFrame = false;
    }

    bool GlowReader::read(const osg::Node& objectBase, float outColour[3], std::size_t& outTextureIndex)
    {
        const osg::StateSet* stateSet = findGlowStateSet(objectBase, 0);
        if (stateSet == nullptr)
            return false;

        // The colour, straight off the uniform GlowUpdater wrote, rather than derived from the
        // enchantment's first effect the way MWWorld::Class::getEnchantmentColor derives it. Same
        // number by construction, but this one also follows a spell-cast glow, which overwrites
        // mColor for a second and a half and then puts the enchant colour back (util.cpp line 133).
        // Deriving it would leave a lock spell tinting nothing.
        osg::Vec4f colour(1.0f, 1.0f, 1.0f, 1.0f);
        if (const osg::Uniform* uniform = stateSet->getUniform("envMapColor"))
            uniform->get(colour);

        // Gamma, and deliberately NOT put through Vk::srgbToLinear on the way past. Every other
        // authored colour that enters this renderer is decoded here and this one is not, so it
        // looks like the omission the rest of the colour path exists to prevent. It is not: this
        // colour is multiplied by the caustic sample in *gamma* space in glow.frag, exactly the way
        // objects.frag line 202 does it, and the product is decoded there. Decoding it here as well
        // would decode it twice and the glow would come out roughly a third as bright as OSG's.
        outColour[0] = colour.x();
        outColour[1] = colour.y();
        outColour[2] = colour.z();

        // Which caustic frame is up, read rather than recomputed. GlowUpdater picks it with
        // int(simulationTime * 16) % 32 (util.cpp line 116), and repeating that here would be a
        // second clock: it would keep running while the game is paused, and it would drift from the
        // one OSG is using the moment either side changed its time source.
        std::size_t textureIndex = static_cast<std::size_t>(-1);
        const osg::StateSet::TextureAttributeList& units = stateSet->getTextureAttributeList();
        for (unsigned int unit = 0; unit < units.size(); ++unit)
        {
            const auto* texture = dynamic_cast<const osg::Texture2D*>(
                stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
            if (texture == nullptr || texture->getImage() == nullptr)
                continue;

            const std::string& name = texture->getImage()->getFileName();
            if (!isCausticFrame(name))
                continue;

            textureIndex = mResolveTexture(name);
            if (textureIndex != static_cast<std::size_t>(-1)
                && std::find(mCausticIndices.begin(), mCausticIndices.end(), textureIndex)
                    == mCausticIndices.end())
            {
                mCausticIndices.push_back(textureIndex);
            }

            // Set here, where the caustic is found, and not beside the successful return below.
            // The flag means "something in the loaded cells is enchanted this frame", which is the
            // question collectLiveTextures is asking when it decides whether to keep the flipbook
            // resident. Setting it below made it mean "a glow was drawn this frame", which is a
            // different question and one that could never become true.
            //
            // It deadlocked, and this is why all 32 caustics answered the white fallback forever:
            // textureIndices() reported nothing while the flag was clear, so the frames were never
            // in the live set, so they were never given a sampler slot, so the slot came back 0, so
            // read() returned false and never set the flag. Nothing about how often the texture
            // sync ran could break that loop -- it was closed inside this class.
            //
            // The loop cannot form at all now, because nothing here tests a slot any more and the
            // return below no longer depends on one. That is worth knowing before anyone moves this
            // line back down: the flag is still what keeps the flipbook resident, which is reason
            // enough for it to be set where the caustic is found rather than where a glow is drawn.
            mAnyGlowThisFrame = true;
            break;
        }

        // The refusal to draw a glow on the white fallback is still in force -- it would add a
        // solid object-shaped block of the enchantment colour over the scene, far more visible than
        // the glow it stands in for -- but it cannot be made here any more, because no sampler slot
        // exists until the caller's texture sync has run and that is later in the frame than this.
        // It is made at the far end instead: the caller resolves this index to a slot when it
        // submits the instance, and Renderer::render already skips any submission whose glow slot
        // is 0. Moving it there also fixed the frame it used to be wrong on -- a slot resolved here
        // and stored survived the sync's renumbering as a number naming a different texture.
        //
        // What is still refused here is the case that is knowable here: a caustic stateset was
        // found, but its file could not be loaded at all.
        if (textureIndex == static_cast<std::size_t>(-1))
            return false;

        outTextureIndex = textureIndex;
        return true;
    }

    const std::vector<std::size_t>& GlowReader::textureIndices() const
    {
        return mAnyGlowThisFrame ? mCausticIndices : mEmpty;
    }
}

#endif
