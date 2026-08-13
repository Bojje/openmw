#ifndef OPENMW_MWLUA_WORKER_H
#define OPENMW_MWLUA_WORKER_H

#include <osg/Timer>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

#include <components/render/stats.hpp>

namespace MWLua
{
    class LuaManager;

    class Worker
    {
    public:
        explicit Worker(LuaManager& manager);

        ~Worker();

        void allowUpdate(osg::Timer_t frameStart, unsigned int frameNumber, Render::FrameStats& stats);

        void finishUpdate(osg::Timer_t frameStart, unsigned int frameNumber, Render::FrameStats& stats);

        void join();

    private:
        struct UpdateRequest
        {
            osg::Timer_t mFrameStart;
            unsigned mFrameNumber;
            Render::FrameStats* mStats;
        };

        void update(osg::Timer_t frameStart, unsigned frameNumber, Render::FrameStats& stats);

        void run() noexcept;

        LuaManager& mManager;
        std::mutex mMutex;
        std::condition_variable mCV;
        std::optional<UpdateRequest> mUpdateRequest;
        bool mJoinRequest = false;
        std::optional<std::thread> mThread;
    };
}

#endif // OPENMW_MWLUA_LUAWORKER_H
