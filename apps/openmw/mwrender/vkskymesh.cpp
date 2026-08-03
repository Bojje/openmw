#ifdef OPENMW_USE_VULKAN

#include "vkskymesh.hpp"

#include <osg/Array>
#include <osg/Geometry>
#include <osg/TriangleIndexFunctor>
#include <osg/ref_ptr>

#include <components/debug/debuglog.hpp>
#include <components/vk/vkbuffer.hpp>
#include <components/vk/vkgeometry.hpp>

namespace
{
    /// Collects every triangle a geometry draws, whatever it draws them as.
    ///
    /// osg::TriangleIndexFunctor is what makes this three lines instead of a switch over primitive
    /// modes and index widths. The sky NIFs are plain indexed triangle lists today, but
    /// Resource::SceneManager runs MERGE_GEOMETRY over everything it loads and a mod can replace either
    /// mesh, so the set of modes that can actually turn up here is not the set the vanilla files use.
    /// The functor covers strips, fans, quads and polygons alike and reads the index arrays whatever
    /// their width, which is the difference between "unusual primitive" and "the clouds are missing".
    struct TriangleCollector
    {
        std::vector<uint32_t>* mIndices = nullptr;

        void operator()(unsigned int i1, unsigned int i2, unsigned int i3)
        {
            mIndices->push_back(static_cast<uint32_t>(i1));
            mIndices->push_back(static_cast<uint32_t>(i2));
            mIndices->push_back(static_cast<uint32_t>(i3));
        }
    };

    /// Rewrites the geometry's arrays into the 48-byte interleaved vertex the rest of this renderer
    /// draws with. False if there is nothing usable here.
    bool buildInterleaved(
        const osg::Geometry& geometry, std::vector<float>& vertices, std::vector<uint32_t>& indices)
    {
        const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
        if (positions == nullptr || positions->empty())
            return false;

        const std::size_t count = positions->size();

        // Sizes are checked against the vertex count rather than trusted. A colour array bound
        // BIND_OVERALL holds one element, and indexing it per vertex would walk off the end of a
        // one-element array on the second vertex.
        const auto* normals = dynamic_cast<const osg::Vec3Array*>(geometry.getNormalArray());
        if (normals != nullptr && normals->size() != count)
            normals = nullptr;

        const auto* uvs = dynamic_cast<const osg::Vec2Array*>(geometry.getTexCoordArray(0));
        if (uvs != nullptr && uvs->size() != count)
            uvs = nullptr;

        const auto* colours = dynamic_cast<const osg::Vec4Array*>(geometry.getColorArray());
        if (colours != nullptr && colours->size() != count)
            colours = nullptr;

        vertices.assign(count * Vk::sFloatsPerVertex, 0.f);

        for (std::size_t i = 0; i < count; ++i)
        {
            float* dst = vertices.data() + i * Vk::sFloatsPerVertex;

            const osg::Vec3f& position = (*positions)[i];
            dst[0] = position.x();
            dst[1] = position.y();
            dst[2] = position.z();

            // Written even though no sky shader reads it, because the interleave is shared with the
            // G-buffer and a stride that disagrees with Vk::sVertexStride would misread every
            // following attribute. Up, arbitrarily: this is a sky dome lit by nothing.
            if (normals != nullptr)
            {
                const osg::Vec3f& normal = (*normals)[i];
                dst[3] = normal.x();
                dst[4] = normal.y();
                dst[5] = normal.z();
            }
            else
            {
                dst[5] = 1.f;
            }

            if (uvs != nullptr)
            {
                const osg::Vec2f& uv = (*uvs)[i];
                dst[6] = uv.x();
                dst[7] = uv.y();
            }

            // Only the alpha is ever read -- paintClouds and paintAtmosphereNight both use passColor.a
            // and nothing else -- and ModVertexAlphaVisitor writes (0, 0, 0, alpha) into every one of
            // these anyway (skyutil.cpp line 1084). So the rgb deliberately does NOT go through
            // Vk::srgbToLinear the way NifVk::MeshConverter's does: decoding a zero gives a zero, and
            // pretending otherwise would put a colour-space conversion on a channel nothing samples.
            if (colours != nullptr)
            {
                const osg::Vec4f& colour = (*colours)[i];
                dst[8] = colour.r();
                dst[9] = colour.g();
                dst[10] = colour.b();
                dst[11] = colour.a();
            }
            else
            {
                dst[8] = 1.f;
                dst[9] = 1.f;
                dst[10] = 1.f;
                dst[11] = 1.f;
            }
        }

        osg::TriangleIndexFunctor<TriangleCollector> collector;
        collector.mIndices = &indices;
        geometry.accept(collector);

        return !indices.empty();
    }
}

namespace MWRender
{
    struct SkyMeshCache::Entry
    {
        // A reference, not a bare pointer, and that is the whole reason this struct exists. The cache
        // is keyed on the address of the osg::Geometry; if the sky were ever torn down and rebuilt, a
        // new drawable could land on the freed address and be handed the previous mesh's buffers --
        // which fails as the wrong shape in the sky rather than as a crash, and would be blamed on the
        // NIF. Holding the drawable alive makes the address unique for as long as the key exists. It
        // costs four drawables that the sky is holding anyway.
        osg::ref_ptr<const osg::Geometry> source;
        Vk::Geometry geometry;
    };

    SkyMeshCache::SkyMeshCache(Vk::Device& device, Vk::CommandPool& commandPool)
        : mDevice(device)
        , mCommandPool(commandPool)
    {
    }

    SkyMeshCache::~SkyMeshCache() = default;

    SkyMeshBuffers SkyMeshCache::getOrUpload(const osg::Geometry& geometry)
    {
        const auto describe = [](const Entry& entry) {
            SkyMeshBuffers buffers;
            if (!entry.geometry.valid())
                return buffers;

            buffers.vertexBuffer = entry.geometry.vertexBuffer->handle();
            buffers.indexBuffer = entry.geometry.indexBuffer->handle();
            buffers.indexCount = entry.geometry.indexCount;
            return buffers;
        };

        for (const std::unique_ptr<Entry>& entry : mEntries)
        {
            if (entry->source.get() == &geometry)
                return describe(*entry);
        }

        std::vector<float> vertices;
        std::vector<uint32_t> indices;

        auto entry = std::make_unique<Entry>();
        entry->source = &geometry;

        // The failed conversion is cached too, as an entry with no buffers. Without that, a sky mesh
        // this cannot read is re-read, re-converted and re-logged every frame for the rest of the
        // session -- and the conversion walks every vertex.
        if (buildInterleaved(geometry, vertices, indices))
        {
            const uint32_t vertexCount = static_cast<uint32_t>(vertices.size() / Vk::sFloatsPerVertex);

            // alphaTested false. It only decides whether the BLAS marks the geometry opaque, and the
            // sky is never submitted to the TLAS: it is drawn in the composite pass, after the ray
            // tracing is finished, and a cloud layer wrapped around the camera would occlude every
            // shadow and reflection ray in the world if it were in the acceleration structure.
            //
            // The BLAS that uploadGeometry builds anyway is therefore dead weight. It is left alone
            // because it is two small meshes built once at startup, and because giving the sky its own
            // upload path to avoid it would be a second copy of the staging and barrier code -- which
            // is a far more expensive thing to get subtly wrong.
            entry->geometry = Vk::uploadGeometry(mDevice, mCommandPool, vertices.data(), vertexCount,
                indices.data(), static_cast<uint32_t>(indices.size()), false);
        }
        else
        {
            Log(Debug::Warning) << "Vulkan: sky mesh '" << geometry.getName()
                                << "' has no readable triangles; it will not be drawn";
        }

        mEntries.push_back(std::move(entry));
        return describe(*mEntries.back());
    }
}

#endif
