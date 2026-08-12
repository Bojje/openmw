#include "sdlcursormanager.hpp"

#include <stdexcept>

#include <SDL_endian.h>
#include <SDL_hints.h>
#include <SDL_mouse.h>
#include <SDL_render.h>

#include <components/debug/debuglog.hpp>
#include <components/render/texture.hpp>

#include "imagetosurface.hpp"

namespace SDLUtil
{

    SDLCursorManager::SDLCursorManager()
        : mEnabled(false)
        , mInitialized(false)
    {
    }

    SDLCursorManager::~SDLCursorManager()
    {
        CursorMap::const_iterator cursIter = mCursorMap.begin();

        while (cursIter != mCursorMap.end())
        {
            SDL_FreeCursor(cursIter->second);
            ++cursIter;
        }

        mCursorMap.clear();
    }

    void SDLCursorManager::setEnabled(bool enabled)
    {
        if (mInitialized && enabled == mEnabled)
            return;

        mInitialized = true;
        mEnabled = enabled;

        // turn on hardware cursors
        if (enabled)
        {
            _setGUICursor(mCurrentCursor);
        }
        // turn off hardware cursors
        else
        {
            SDL_ShowCursor(SDL_FALSE);
        }
    }

    void SDLCursorManager::cursorChanged(std::string_view name)
    {
        mCurrentCursor = name;
        _setGUICursor(mCurrentCursor);
    }

    void SDLCursorManager::_setGUICursor(std::string_view name)
    {
        auto it = mCursorMap.find(name);
        if (it != mCursorMap.end())
            SDL_SetCursor(it->second);
    }

    void SDLCursorManager::createCursor(std::string_view name, int rotDegrees, const Render::TextureData& image,
        Uint8 hotspotX,
        Uint8 hotspotY, int cursorWidth, int cursorHeight)
    {
#ifndef ANDROID
        _createCursorFromResource(name, rotDegrees, image, hotspotX, hotspotY, cursorWidth, cursorHeight);
#endif
    }

    SDLUtil::SurfaceUniquePtr decompress(
        const Render::TextureData& source, float rotDegrees, int cursorWidth, int cursorHeight)
    {
        if (!source.valid())
            return { nullptr, SDL_FreeSurface };

        const int width = static_cast<int>(source.width);
        const int height = static_cast<int>(source.height);

        Uint32 redMask = 0x000000ff;
        Uint32 greenMask = 0x0000ff00;
        Uint32 blueMask = 0x00ff0000;
        Uint32 alphaMask = 0xff000000;

        SDL_Surface* cursorSurface = SDL_CreateRGBSurfaceFrom(const_cast<Uint8*>(source.pixels.data()), width, height,
            32, width * 4, redMask, greenMask,
            blueMask, alphaMask);

        SDL_Surface* targetSurface
            = SDL_CreateRGBSurface(0, cursorWidth, cursorHeight, 32, redMask, greenMask, blueMask, alphaMask);
        SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(targetSurface);

        SDL_RenderClear(renderer);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_Texture* cursorTexture = SDL_CreateTextureFromSurface(renderer, cursorSurface);

        SDL_RenderCopyEx(renderer, cursorTexture, nullptr, nullptr, -rotDegrees, nullptr, SDL_FLIP_NONE);

        SDL_DestroyTexture(cursorTexture);
        SDL_FreeSurface(cursorSurface);
        SDL_DestroyRenderer(renderer);

        return SDLUtil::SurfaceUniquePtr(targetSurface, SDL_FreeSurface);
    }

    void SDLCursorManager::_createCursorFromResource(std::string_view name, int rotDegrees,
        const Render::TextureData& image,
        Uint8 hotspotX, Uint8 hotspotY, int cursorWidth, int cursorHeight)
    {
        if (mCursorMap.find(name) != mCursorMap.end())
            return;

        try
        {
            auto surface = decompress(image, static_cast<float>(rotDegrees), cursorWidth, cursorHeight);

            // set the cursor and store it for later
            SDL_Cursor* curs = SDL_CreateColorCursor(surface.get(), hotspotX, hotspotY);

            mCursorMap.emplace(name, curs);
        }
        catch (std::exception& e)
        {
            Log(Debug::Warning) << e.what();
            Log(Debug::Warning) << "Using default cursor.";
            return;
        }
    }

}
