#include "engine.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <future>
#include <fstream>
#include <system_error>

#include <osgGA/GUIEventAdapter>
#include <osg/Stats>
#include <osg/Timer>
#include <osg/Version>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <SDL.h>
#include <SDL_vulkan.h>

#include <components/debug/debuglog.hpp>
#include <components/misc/rng.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/nifmeshmanager.hpp>
#include <components/resource/neutraltexturemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>
#include <components/compiler/extensions0.hpp>
#include <components/render/imagewriter.hpp>
#include <components/render/texture.hpp>
#ifdef OPENMW_NEUTRAL_JPEG
#include <components/render/jpeg.hpp>
#endif
#ifdef OPENMW_NEUTRAL_PNG
#include <components/render/png.hpp>
#endif
#include <components/render/math.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/glextensions.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>

#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"
#include "mwgui/nullwindowmanager.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/renderingmanager.hpp"
#include "mwrender/vismask.hpp"
#include "mwrender/viewerframelifecycle.hpp"
#include "mwrender/vulkanframelifecycle.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

#ifndef OPENMW_VULKAN_SHADER_DIR
#define OPENMW_VULKAN_SHADER_DIR ""
#endif

namespace
{
    Render::Mat4 neutralLookAt(const Render::Vec3& eye, const Render::Vec3& center, const Render::Vec3& upHint)
    {
        const Render::Vec3 forward = { center.x - eye.x, center.y - eye.y, center.z - eye.z };
        const float length = std::sqrt(forward.x * forward.x + forward.y * forward.y + forward.z * forward.z);
        if (length <= 0.f)
            return Render::identityMat4();
        const Render::Vec3 f = { forward.x / length, forward.y / length, forward.z / length };
        Render::Vec3 side = { f.y * upHint.z - f.z * upHint.y, f.z * upHint.x - f.x * upHint.z,
            f.x * upHint.y - f.y * upHint.x };
        const float fullSideLength = std::sqrt(side.x * side.x + side.y * side.y + side.z * side.z);
        if (fullSideLength <= 0.f)
            side = { 1.f, 0.f, 0.f };
        else
            side = { side.x / fullSideLength, side.y / fullSideLength, side.z / fullSideLength };
        const Render::Vec3 up = { side.y * f.z, -side.x * f.z, side.x * f.y - side.y * f.x };

        Render::Mat4 result = Render::identityMat4();
        result.data[0] = side.x;
        result.data[1] = up.x;
        result.data[2] = -f.x;
        result.data[4] = side.y;
        result.data[5] = up.y;
        result.data[6] = -f.y;
        result.data[8] = side.z;
        result.data[9] = up.z;
        result.data[10] = -f.z;
        result.data[12] = -(side.x * eye.x + side.y * eye.y + side.z * eye.z);
        result.data[13] = -(up.x * eye.x + up.y * eye.y + up.z * eye.z);
        result.data[14] = f.x * eye.x + f.y * eye.y + f.z * eye.z;
        return result;
    }

    bool writeVulkanScreenshot(const Render::TextureData& image, const std::filesystem::path& path)
    {
        if (!image.valid())
            return false;

        if (path.extension() == ".jpg")
        {
#ifdef OPENMW_NEUTRAL_JPEG
            std::vector<char> encoded;
            if (!Render::writeJpeg(image, encoded))
                return false;
            std::ofstream output(path, std::ios::binary);
            if (!output)
                return false;
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            return output.good();
#else
            return false;
#endif
        }

        if (path.extension() == ".png")
        {
#ifdef OPENMW_NEUTRAL_PNG
            std::vector<char> encoded;
            if (!Render::writePng(image, encoded))
                return false;
            std::ofstream output(path, std::ios::binary);
            if (!output)
                return false;
            output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
            return output.good();
#else
            return false;
#endif
        }

        if (path.extension() == ".tga")
            return Render::writeTga(image, path);

        return Render::writePpm(image, path);
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

osgViewer::Viewer* OMW::Engine::getOsgViewer() const
{
    const auto* const lifecycle = dynamic_cast<const MWRender::ViewerFrameLifecycle*>(mFrameLifecycle.get());
    return lifecycle ? lifecycle->viewer() : nullptr;
}

osg::Stats* OMW::Engine::getOsgStats() const
{
    const auto* const lifecycle = dynamic_cast<const MWRender::ViewerFrameLifecycle*>(mFrameLifecycle.get());
    return lifecycle ? lifecycle->stats() : mNeutralStats.get();
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = osg::Timer::instance()->tick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = getOsgStats();
    if (!stats)
        throw std::logic_error("Engine frame statistics were not initialized");

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            if (auto* const gui = dynamic_cast<MWGui::WindowManager*>(mWindowManager.get()))
                gui->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    const bool reportResource = stats->collectStats("resource");

    if (reportResource && mUnrefQueue)
        stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

    if (mUnrefQueue && mWorkQueue)
        mUnrefQueue->flush(*mWorkQueue);

    if (reportResource)
    {
        stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

        mResourceSystem->reportStats(frameNumber, stats);

        if (mWorkQueue)
        {
            stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
            stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));
        }

        mMechanicsManager->reportStats(frameNumber, *stats);
        mWorld->reportStats(frameNumber, *stats);
        mLuaManager->reportStats(frameNumber, *stats);

        stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
    }

    if (mStereoManager)
        mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
        mWorld->updateFocusObject();
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    const auto finishLuaUpdate = [this, &frameStart, frameNumber, stats] {
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
    };
    bool rendered = false;
    try
    {
        rendered = mWorld->renderFrame();
    }
    catch (...)
    {
        finishLuaUpdate();
        throw;
    }

    finishLuaUpdate();

    if (!rendered)
        return false;

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mUseVulkan(false)
    , mCfgMgr(configurationManager)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;
    mNeutralStats = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mFrameLifecycle = nullptr;
    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::prepareVulkanEngine()
{
    mNeutralStats = new osg::Stats("OpenMW Vulkan");

    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    mVFS = std::make_unique<VFS::Manager>();
    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());
    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder(),
        Resource::ResourceSystem::Backend::Neutral);
    mEnvironment.setResourceSystem(*mResourceSystem);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);
    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    const bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";
    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;
    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }

    mWindowManager = std::make_unique<MWGui::NullWindowManager>(Version::getOpenmwVersionDescription(), [this] {
        mFrameLifecycle->requestQuit();
    });
    mEnvironment.setWindowManager(*mWindowManager);

    SDLUtil::InputCallbacks inputCallbacks;
    inputCallbacks.frame = [] {};
    inputCallbacks.functionKey = [](int, bool) {};
    inputCallbacks.resize = [this](int, int, int width, int height) {
        mFrameLifecycle->resize();
        mWindowManager->windowResized(width, height);
    };
    const auto screenshot = [this] {
        const std::optional<Render::TextureData> image = mFrameLifecycle->captureFrame();
        if (!image)
        {
            Log(Debug::Warning) << "Vulkan screenshot requested before a frame was presented";
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(mCfgMgr.getScreenshotPath(), error);
        const auto stamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string screenshotFormat = Settings::general().mScreenshotFormat.get();
        const bool jpeg = screenshotFormat == "jpg";
        const bool png = screenshotFormat == "png";
        const bool tga = screenshotFormat == "tga";
        const std::filesystem::path path = mCfgMgr.getScreenshotPath()
            / ("openmw-vulkan-" + std::to_string(stamp)
                + (jpeg ? ".jpg" : png ? ".png" : tga ? ".tga" : ".ppm"));
        if (!writeVulkanScreenshot(*image, path))
            Log(Debug::Warning) << "Failed to write Vulkan screenshot " << path;
        else
            Log(Debug::Info) << "Vulkan screenshot written to " << path;
    };
    mInputManager = std::make_unique<MWInput::InputManager>(mWindow, std::move(inputCallbacks), screenshot, keybinderUser,
        keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);
    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);
    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = mWindowManager->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });
    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->initSimulation(mMaxRecastLogLevel, mFrameLifecycle->backend());
    const Render::MeshResolver meshResolver = [resourceSystem = mResourceSystem.get()](std::string_view model) {
        const VFS::Path::Normalized path(model);
        if (path.extension().value() == "nif")
            return resourceSystem->getNifMeshManager()->get(path);
        return std::make_shared<const std::vector<Render::MeshInstance>>();
    };
    Resource::NeutralTextureManager* const textureManager = mResourceSystem->getNeutralTextureManager();
    const Render::TextureResolver textureResolver = [textureManager](std::string_view name) {
        if (name.empty())
            return std::shared_ptr<const Render::TextureData>();
        return textureManager->get(VFS::Path::Normalized(name));
    };
    const Render::PoseResolver poseResolver = [resourceSystem = mResourceSystem.get()](
                                                 std::string_view model, float time,
                                                 std::span<const std::string> boneNames) {
        const VFS::Path::Normalized path(model);
        if (path.extension().value() != "nif")
            return std::vector<Render::Mat4>();
        std::vector<Render::Mat4> pose = resourceSystem->getNifMeshManager()->getBonePose(path, time, boneNames);
        if (!pose.empty())
            return pose;

        VFS::Path::Normalized kfPath(path);
        kfPath.changeExtension(VFS::Path::ExtensionView("kf"));
        if (!resourceSystem->getVFS()->exists(kfPath))
            return pose;
        return resourceSystem->getNifMeshManager()->getBonePose(
            resourceSystem->getNifFileManager()->get(kfPath), time, boneNames);
    };
    const Render::SceneSynchronizer sceneSynchronizer = [this](Render::SceneData& sceneData) {
        mWorld->updateNeutralSceneData(sceneData);
        int drawableWidth = Settings::video().mResolutionX.get();
        int drawableHeight = Settings::video().mResolutionY.get();
        if (mWindow)
            SDL_Vulkan_GetDrawableSize(mWindow, &drawableWidth, &drawableHeight);
        const float aspect = static_cast<float>(std::max(1, drawableWidth))
            / static_cast<float>(std::max(1, drawableHeight));
        const MWWorld::Ptr player = mWorld->getPlayerPtr();
        const bool firstPerson = mWorld->isFirstPerson();
        const float fieldOfView = firstPerson ? Settings::camera().mFirstPersonFieldOfView.get()
                                              : Settings::camera().mFieldOfView.get();
        sceneData.projection = Render::perspective(aspect, fieldOfView, Settings::camera().mNearClip.get(),
            Settings::camera().mViewingDistance.get());
        if (player.isEmpty())
            return;
        const ESM::Position& position = player.getRefData().getPosition();
        const bool vanity = mWorld->isNeutralVanityModeEnabled();
        const Render::Vec3 playerPosition{ position.pos[0], position.pos[1], position.pos[2] };
        const Render::CameraPose camera = Render::makeCameraPose(playerPosition,
            { position.rot[0], position.rot[1], position.rot[2] }, firstPerson, vanity,
            mWorld->getNeutralVanityPitch(), mWorld->getNeutralVanityYaw());
        sceneData.view = neutralLookAt(camera.eye, camera.target, camera.up);
        sceneData.viewInverse = Render::invertMat4(sceneData.view);
        sceneData.projInverse = Render::invertMat4(sceneData.projection);
    };
    mWorld->initNeutralRenderer(
        *mFrameLifecycle, sceneSynchronizer, std::move(meshResolver), textureResolver, poseResolver);
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    mLuaManager->initPostLoad();

    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

void OMW::Engine::prepareEngine()
{
    if (mUseVulkan)
    {
        prepareVulkanEngine();
        return;
    }

    if (mFrameLifecycle->backend() != Render::FrameLifecycle::Backend::Osg)
        throw std::logic_error("The full game currently requires the OSG frame lifecycle");

    auto* const viewerLifecycle = dynamic_cast<MWRender::ViewerFrameLifecycle*>(mFrameLifecycle.get());
    if (!viewerLifecycle)
        throw std::logic_error("OSG engine setup requires the OSG frame lifecycle");
    osgViewer::Viewer* const viewer = viewerLifecycle->viewer();

    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled
        = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        viewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode = viewerLifecycle->sceneRoot();
    viewerLifecycle->initializeWindow(mWindow, mResDir);

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), Settings::cells().mCacheExpiryDelay, &mEncoder.get()->getStatelessEncoder(),
        Resource::ResourceSystem::Backend::Osg);
    if (Resource::SceneManager* const sceneManager = mResourceSystem->getSceneManager())
    {
        sceneManager->getShaderManager().setMaxTextureUnits(viewerLifecycle->maxTextureImageUnits());
        sceneManager->setUnRefImageDataAfterApply(false); // keep to Off for now to allow better state sharing
        sceneManager->setFilterSettings(Settings::general().mTextureMagFilter, Settings::general().mTextureMinFilter,
            Settings::general().mTextureMipmap, static_cast<float>(Settings::general().mAnisotropy));
    }
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);
    viewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    if (Resource::SceneManager* const sceneManager = mResourceSystem->getSceneManager())
        sceneManager->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    if (mStereoManager)
        mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, viewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), mCfgMgr, [this] {
            if (!mWorld || !mWorld->renderFrame())
                mFrameLifecycle->renderFrame();
        }, [this] {
            const double simulationTime = mFrameLifecycle->referenceTime();
            if (!mWorld || !mWorld->advanceFrame(simulationTime))
                mFrameLifecycle->advanceFrame(simulationTime);
        });
    mEnvironment.setWindowManager(*mWindowManager);

    SDLUtil::InputCallbacks inputCallbacks;
    inputCallbacks.frame = [viewer] { viewer->getEventQueue()->frame(0.f); };
    inputCallbacks.functionKey = [viewer](int key, bool pressed) {
        const int osgKey = osgGA::GUIEventAdapter::KEY_F1 + (key - SDLK_F1);
        if (pressed)
            viewer->getEventQueue()->keyPress(osgKey);
        else
            viewer->getEventQueue()->keyRelease(osgKey);
    };
    inputCallbacks.resize = [viewer](int x, int y, int width, int height) {
        if (osg::GraphicsContext* const context = viewer->getCamera()->getGraphicsContext())
            context->resized(x, y, width, height);
        viewer->getEventQueue()->windowResize(x, y, width, height);
    };
    mInputManager = std::make_unique<MWInput::InputManager>(mWindow, std::move(inputCallbacks), [this] {
        osgViewer::Viewer* const captureViewer = getOsgViewer();
        if (!mScreenCaptureHandler || !captureViewer)
            return;
        mScreenCaptureHandler->setFramesToCapture(1);
        mScreenCaptureHandler->captureNextFrame(*captureViewer);
    }, keybinderUser,
        keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    if (auto* const gui = dynamic_cast<MWGui::WindowManager*>(mWindowManager.get()))
        gui->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    listener->loadingOff();

    mWorld->initSimulation(mMaxRecastLogLevel, mFrameLifecycle->backend());
    mWorld->initOsgRenderer(viewer, *mFrameLifecycle, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    if (auto* const gui = dynamic_cast<MWGui::WindowManager*>(mWindowManager.get()))
        gui->initUI();
    mLuaManager->initPostLoad();

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    if (!mUseVulkan)
        Log(Debug::Info) << "OSG version: " << osgGetVersion();
    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    if (mUseVulkan)
    {
#ifndef OPENMW_USE_VULKAN
        throw std::logic_error("--vulkan requires an OpenMW build configured with OPENMW_USE_VULKAN=ON");
#else
        const int screen = Settings::video().mScreen;
        const int width = Settings::video().mResolutionX;
        const int height = Settings::video().mResolutionY;
        int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
        Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
        if (Settings::video().mWindowMode == Settings::WindowMode::Fullscreen)
        {
            flags |= SDL_WINDOW_FULLSCREEN;
            posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
            posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        }
        else if (Settings::video().mWindowMode == Settings::WindowMode::WindowedFullscreen)
        {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
            posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        }
        if (!Settings::video().mWindowBorder)
            flags |= SDL_WINDOW_BORDERLESS;
        mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
        if (!mWindow)
            throw std::runtime_error(std::string("Failed to create Vulkan SDL window: ") + SDL_GetError());

        mFrameLifecycle = std::make_unique<MWRender::VulkanFrameLifecycle>(mWindow, OPENMW_VULKAN_SHADER_DIR);
#endif
    }
    else
        mFrameLifecycle = std::make_unique<MWRender::ViewerFrameLifecycle>();

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

    std::filesystem::path path;
    std::ofstream stats;
    if (!mUseVulkan)
    {
#ifdef _WIN32
        const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
        const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif
        if (statsFile != nullptr)
            path = statsFile;

        if (!path.empty())
        {
            stats.open(path, std::ios_base::out);
            if (stats.is_open())
                Log(Debug::Info) << "OSG stats will be written to: " << path;
            else
                Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                    << "\": " << std::generic_category().message(errno);
        }

        if (auto* const statsLifecycle = dynamic_cast<MWRender::ViewerFrameLifecycle*>(mFrameLifecycle.get()))
            statsLifecycle->initializeStatsHandlers(*mVFS, stats.is_open(), initStatsHandler);
    }

    // Start the game
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu && !mUseVulkan)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        if (mUseVulkan)
        {
            Log(Debug::Warning) << "Vulkan mode has no GUI yet; starting a bypassed new game";
            mStateManager->newGame(true);
        }
        else
            mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mFrameLifecycle->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        mWorld->advanceFrame(timeManager.getRenderingSimulationTime());

        const unsigned frameNumber = mFrameLifecycle->frameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        auto* const reportingLifecycle = dynamic_cast<MWRender::ViewerFrameLifecycle*>(mFrameLifecycle.get());
        if (stats && reportingLifecycle)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mFrameLifecycle->frameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportingLifecycle->reportStats(i - statsReportDelay, stats);
            }
        }

        frameRateLimiter.limit();
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}
