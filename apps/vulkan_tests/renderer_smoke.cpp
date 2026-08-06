#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <components/render/imagecomparison.hpp>
#include <components/render/mesh.hpp>
#include <components/vk/vkrenderer.hpp>

#ifndef OPENMW_VULKAN_SHADER_DIR
#define OPENMW_VULKAN_SHADER_DIR ""
#endif

namespace
{
    class EnvironmentUnavailable : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

    Render::Mat4 identityMatrix()
    {
        Render::Mat4 result = {};
        result.data[0] = 1.0f;
        result.data[5] = 1.0f;
        result.data[10] = 1.0f;
        result.data[15] = 1.0f;
        return result;
    }

    unsigned int frameCount(int argc, char** argv)
    {
        if (argc < 3)
            return 3;

        char* end = nullptr;
        const long parsed = std::strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || parsed < 1 || parsed > 100)
            throw std::runtime_error("frame count must be an integer from 1 to 100");
        return static_cast<unsigned int>(parsed);
    }

    std::optional<Render::TextureData> referenceImage(int argc, char** argv)
    {
        if (argc < 4)
            return std::nullopt;
        const auto image = Render::readPpm(argv[3]);
        if (!image)
            throw std::runtime_error(std::string("could not read Vulkan reference image: ") + argv[3]);
        return image;
    }

    std::filesystem::path captureDirectory(int argc, char** argv)
    {
        if (argc < 5)
            return {};
        const std::filesystem::path directory = argv[4];
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error)
            throw std::runtime_error("could not create Vulkan capture directory: " + error.message());
        return directory;
    }

    std::shared_ptr<const std::vector<Render::MeshInstance>> smokeMeshes()
    {
        Render::MeshData data;
        data.vertices = {
            { { -0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f }, { 0.0f, 0.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f }, {}, { 0.0f, 0.0f, 1.0f, 1.0f } },
            { { 0.5f, -0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 1.0f, 0.0f }, { 0.0f, 0.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f }, {}, { 0.0f, 0.0f, 1.0f, 1.0f } },
            { { 0.0f, 0.5f, 0.0f }, { 0.0f, 0.0f, 1.0f }, { 0.5f, 1.0f }, { 0.0f, 0.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f }, {}, { 0.0f, 0.0f, 1.0f, 1.0f } },
        };
        data.indices = { 0, 1, 2 };

        std::vector<Render::MeshInstance> result;
        result.push_back({ data, identityMatrix() });
        result.push_back({ data, identityMatrix() });
        result[0].transform.data[12] = 0.5f;
        result[1].transform.data[12] = 0.75f;
        for (std::size_t i = 0; i < result.size(); ++i)
        {
            Render::MeshInstance& mesh = result[i];
            mesh.mesh.material.albedoTexture = "textures/vulkan-smoke.rgba";
            if (i == 0)
            {
                mesh.mesh.material.alphaBlend = true;
                mesh.mesh.material.diffuse.w = 0.75f;
            }
            else
                mesh.mesh.material.emissiveTexture = "textures/vulkan-smoke-glow.rgba";
        }
        return std::make_shared<const std::vector<Render::MeshInstance>>(std::move(result));
    }

    std::shared_ptr<const Render::TextureData> smokeTexture(std::string_view path)
    {
        if (path != "textures/vulkan-smoke.rgba" && path != "textures/vulkan-smoke-alt.rgba"
            && path != "textures/vulkan-smoke-glow.rgba"
            && path != "textures/vulkan-smoke-normal.rgba")
            throw std::runtime_error("Vulkan smoke requested an unexpected texture");

        auto texture = std::make_shared<Render::TextureData>();
        texture->width = 2;
        texture->height = 2;
        texture->pixels = path == "textures/vulkan-smoke-normal.rgba"
            ? std::vector<uint8_t>{
                  128, 128, 255, 255, 128, 128, 255, 255,
                  128, 128, 255, 255, 128, 128, 255, 255,
              }
            : path == "textures/vulkan-smoke-glow.rgba"
            ? std::vector<uint8_t>{
                  255, 32, 8, 255, 32, 255, 8, 255,
                  8, 32, 255, 255, 255, 255, 32, 255,
              }
            : path == "textures/vulkan-smoke.rgba"
            ? std::vector<uint8_t>{
                  255, 64, 64, 255, 64, 255, 64, 255,
                  64, 64, 255, 255, 255, 255, 255, 255,
              }
            : std::vector<uint8_t>{
                  255, 255, 64, 255, 64, 64, 255, 255,
                  255, 64, 255, 255, 255, 255, 64, 255,
              };
        return texture;
    }

    Render::TerrainTile smokeTerrain()
    {
        Render::TerrainTile tile;
        tile.size = 1.f;
        tile.cellWorldSize = 1.f;
        tile.verticesPerSide = 2;
        tile.vertices = {
            { { -0.5f, -0.5f, -0.25f }, { 0.f, 0.f, 1.f }, { 255, 255, 255, 255 } },
            { { 0.5f, -0.5f, -0.25f }, { 0.f, 0.f, 1.f }, { 255, 255, 255, 255 } },
            { { -0.5f, 0.5f, -0.25f }, { 0.f, 0.f, 1.f }, { 255, 255, 255, 255 } },
            { { 0.5f, 0.5f, -0.25f }, { 0.f, 0.f, 1.f }, { 255, 255, 255, 255 } },
        };
        tile.indices = { 0, 2, 1, 1, 2, 3 };
        Render::TextureData firstBlendmap;
        firstBlendmap.width = 2;
        firstBlendmap.height = 2;
        firstBlendmap.pixels = {
            255, 255, 255, 255, 255, 255, 255, 255,
            255, 255, 255, 255, 255, 255, 255, 255,
        };
        Render::TextureData secondBlendmap = firstBlendmap;
        secondBlendmap.pixels = {
            255, 255, 255, 255, 128, 255, 255, 128,
            255, 255, 255, 128, 255, 255, 255, 128,
        };
        Render::TerrainLayer firstLayer;
        firstLayer.diffuseTexture = "textures/vulkan-smoke.rgba";
        firstLayer.normalTexture = "textures/vulkan-smoke-normal.rgba";
        firstLayer.parallax = true;
        firstLayer.specular = true;
        firstLayer.blendmap = std::move(firstBlendmap);
        Render::TerrainLayer secondLayer;
        secondLayer.diffuseTexture = "textures/vulkan-smoke.rgba";
        secondLayer.normalTexture = "textures/vulkan-smoke-normal.rgba";
        secondLayer.parallax = true;
        secondLayer.specular = true;
        secondLayer.blendmap = std::move(secondBlendmap);
        tile.layers = { std::move(firstLayer), std::move(secondLayer) };
        return tile;
    }
}

int main(int argc, char** argv)
{
    const std::string shaderDir = argc >= 2 ? argv[1] : OPENMW_VULKAN_SHADER_DIR;
    if (shaderDir.empty())
    {
        std::cerr << "Vulkan smoke test requires a shader output directory\n";
        return EXIT_FAILURE;
    }

    SDL_Window* window = nullptr;
    try
    {
        const unsigned int frames = frameCount(argc, argv);
        const auto reference = referenceImage(argc, argv);
        const std::filesystem::path captures = captureDirectory(argc, argv);
        const auto meshes = smokeMeshes();
        const auto texture = smokeTexture("textures/vulkan-smoke.rgba");
        if (!texture || !texture->valid())
            throw std::runtime_error("Vulkan smoke texture resolver returned invalid data");

        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            throw EnvironmentUnavailable(std::string("SDL initialization failed: ") + SDL_GetError());

        window = SDL_CreateWindow("OpenMW Vulkan smoke test",
            SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 640, 480,
            SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window)
            throw EnvironmentUnavailable(std::string("SDL Vulkan window creation failed: ") + SDL_GetError());

        int drawableWidth = 0;
        int drawableHeight = 0;
        SDL_Vulkan_GetDrawableSize(window, &drawableWidth, &drawableHeight);
        if (drawableWidth <= 0 || drawableHeight <= 0)
            throw EnvironmentUnavailable("SDL returned an unavailable Vulkan drawable size");

        {
            auto renderer = std::make_unique<Vk::Renderer>(window, true);
            if (!renderer->validationEnabled())
                throw std::runtime_error("Vulkan smoke test requires validation layers");
            if (!renderer->loadShadersAndCreatePipelines(shaderDir))
                throw std::runtime_error("Vulkan smoke test could not load the raster shaders");

            Render::SceneData scene = {};
            scene.view = identityMatrix();
            scene.projection = identityMatrix();
            scene.viewInverse = identityMatrix();
            scene.projInverse = identityMatrix();
            scene.sunDirection = { 0.0f, -1.0f, 0.0f, 0.0f };
            scene.sunColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            scene.ambientColor = { 0.15f, 0.15f, 0.15f, 1.0f };

            Render::SceneSubmission submission;
            submission.scene = scene;
            submission.meshes = *meshes;
            submission.terrainTiles.push_back(smokeTerrain());
            submission.textureResolver = smokeTexture;
            renderer->setScene(submission);

            // Replace the scene once in the same renderer process. This
            // exercises descriptor growth and per-frame mesh replacement; a
            // reference-image run stays on the single reference checkpoint.
            Render::SceneSubmission alternateSubmission = submission;
            alternateSubmission.meshes[0].mesh.material.albedoTexture = "textures/vulkan-smoke-alt.rgba";
            alternateSubmission.scene.ambientColor = { 0.25f, 0.2f, 0.15f, 1.0f };

            unsigned int renderedFrames = 0;
            std::optional<Render::TextureData> previousCapture;
            for (unsigned int frame = 0; frame < frames; ++frame)
            {
                if (!reference && frames > 2 && frame == frames / 2)
                {
                    renderer->setScene(alternateSubmission);
                    previousCapture.reset();
                }

                SDL_PumpEvents();
                if (renderer->render())
                {
                    ++renderedFrames;
                    const std::optional<Render::TextureData> capture = renderer->captureFrame();
                    if (!capture || !capture->valid())
                        throw std::runtime_error("Vulkan smoke could not capture its rendered frame");
                    if (reference)
                    {
                        const Render::ImageComparison comparison = Render::compareImages(*reference, *capture, 1);
                        if (!comparison.matches(1))
                            throw std::runtime_error("Vulkan smoke frame did not match the reference image");
                    }
                    if (!captures.empty())
                    {
                        const std::filesystem::path path
                            = captures / ("vulkan-frame-" + std::to_string(frame) + ".ppm");
                        if (!Render::writePpm(*capture, path))
                            throw std::runtime_error("Vulkan smoke could not write its captured frame");
                    }
                    if (previousCapture)
                    {
                        const Render::ImageComparison comparison
                            = Render::compareImages(*previousCapture, *capture, 1);
                        if (!comparison.matches(1))
                            throw std::runtime_error("Vulkan smoke frame capture was not deterministic");
                    }
                    previousCapture = *capture;
                }

                // Recreate the swapchain once without restarting the process. This
                // covers the lifecycle that is most likely to expose ownership bugs.
                if (frame == 0 && frames > 1)
                    renderer->resize(static_cast<uint32_t>(drawableWidth), static_cast<uint32_t>(drawableHeight));
            }

            if (renderedFrames == 0)
                throw EnvironmentUnavailable("Vulkan drawable became unavailable before a frame was submitted");
            if (renderer->validationErrorCount() != 0)
                throw std::runtime_error("Vulkan smoke test received validation errors");
        }

        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
        std::cout << "Vulkan renderer smoke test passed (" << frames << " frames)\n";
        return EXIT_SUCCESS;
    }
    catch (const EnvironmentUnavailable& error)
    {
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::cerr << "Vulkan renderer smoke test skipped: " << error.what() << '\n';
        return 77;
    }
    catch (const std::exception& error)
    {
        if (window)
            SDL_DestroyWindow(window);
        SDL_Quit();
        std::cerr << "Vulkan renderer smoke test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
