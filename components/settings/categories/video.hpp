#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_VIDEO_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_VIDEO_H

#include <components/sdlutil/vsyncmode.hpp>
#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>
#include <components/settings/windowmode.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct VideoCategory : WithIndex
    {
        using WithIndex::WithIndex;

        // Which backend draws the game. Declared unconditionally, including in builds configured
        // without OPENMW_USE_VULKAN: the settings machinery throws at startup if a default in
        // settings-default.cfg has no SettingValue behind it, so this cannot be compiled out.
        // Asking for a backend the build does not have falls back with a warning instead.
        SettingValue<std::string> mRenderer{ mIndex, "Video", "renderer",
            makeEnumSanitizerString({ "opengl", "vulkan" }) };
        SettingValue<int> mResolutionX{ mIndex, "Video", "resolution x", makeMaxSanitizerInt(1) };
        SettingValue<int> mResolutionY{ mIndex, "Video", "resolution y", makeMaxSanitizerInt(1) };
        SettingValue<WindowMode> mWindowMode{ mIndex, "Video", "window mode" };
        SettingValue<int> mScreen{ mIndex, "Video", "screen", makeMaxSanitizerInt(0) };
        SettingValue<bool> mMinimizeOnFocusLoss{ mIndex, "Video", "minimize on focus loss" };
        SettingValue<bool> mWindowBorder{ mIndex, "Video", "window border" };
        SettingValue<int> mAntialiasing{ mIndex, "Video", "antialiasing", makeMaxSanitizerInt(0) };
        SettingValue<SDLUtil::VSyncMode> mVsyncMode{ mIndex, "Video", "vsync mode" };
        SettingValue<float> mFramerateLimit{ mIndex, "Video", "framerate limit", makeMaxSanitizerFloat(0) };
        SettingValue<float> mContrast{ mIndex, "Video", "contrast", makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mGamma{ mIndex, "Video", "gamma", makeMaxStrictSanitizerFloat(0) };
    };
}

#endif
