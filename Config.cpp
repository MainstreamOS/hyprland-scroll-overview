#include "Config.hpp"

#include <algorithm>

#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace {

SScrollOverviewLuaConfig                      g_luaConfig;
ScrollOverview::Config::TOverviewDispatcher  g_overviewDispatcher = nullptr;

// V2 config values. addConfigValue (V1) is a no-op under Hyprland's Lua config
// manager, so plugin keys set via hl.config({plugin={scrolloverview={...}}}) —
// how dots-hyprland and the InterfaceConfig panel configure this plugin — never
// reached the getters. addConfigValueV2 registers through the manager-agnostic
// path; the getters read the live value via ->value().
struct SConfigValues {
    SP<::Config::Values::CIntValue>    gestureDistance;
    SP<::Config::Values::CFloatValue>  scale;
    SP<::Config::Values::CIntValue>    workspaceGap;
    SP<::Config::Values::CIntValue>    wallpaperMode;
    SP<::Config::Values::CStringValue> wallpaperPath;
    SP<::Config::Values::CIntValue>    blur;
    SP<::Config::Values::CIntValue>    shadowEnabled;
    SP<::Config::Values::CIntValue>    shadowRange;
    SP<::Config::Values::CIntValue>    shadowRenderPower;
    SP<::Config::Values::CIntValue>    shadowColor;
};
SConfigValues g_soValues;

int getLegacyPluginIntValueOr(const std::string& name, int fallback) {
    if (::Config::mgr()->type() == ::Config::CONFIG_LUA)
        return fallback;

    const auto VALUE = HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, name);
    if (!VALUE)
        return fallback;

    const auto DATA = reinterpret_cast<Hyprlang::INT* const*>(VALUE->getDataStaticPtr());
    if (!DATA || !*DATA)
        return fallback;

    return sc<int>(**DATA);
}

int overviewLua(lua_State* L) {
    if (!g_overviewDispatcher)
        return luaL_error(L, "overview: dispatcher is not registered");

    const char* arg = "toggle";

    if (lua_gettop(L) >= 1 && !lua_isnoneornil(L, 1)) {
        if (!lua_isstring(L, 1))
            return luaL_error(L, "overview: expected an optional string argument");

        arg = lua_tostring(L, 1);
    }

    const auto result = g_overviewDispatcher(arg);
    if (!result.success)
        return luaL_error(L, "overview: %s", result.error.c_str());

    return 0;
}

int configureLua(lua_State* L) {
    if (!lua_istable(L, 1))
        return luaL_error(L, "configure: expected a table");

    const int TABLE = lua_absindex(L, 1);

    const auto readInt = [L, TABLE](const char* name, std::optional<int>& target) -> bool {
        lua_getfield(L, TABLE, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }

        if (!lua_isinteger(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "configure: %s must be an integer", name);
            return false;
        }

        target = sc<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return true;
    };

    const auto readFloat = [L, TABLE](const char* name, std::optional<float>& target) -> bool {
        lua_getfield(L, TABLE, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }

        if (!lua_isnumber(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "configure: %s must be a number", name);
            return false;
        }

        target = sc<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return true;
    };

    const auto readBool = [L, TABLE](const char* name, std::optional<bool>& target) -> bool {
        lua_getfield(L, TABLE, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            return true;
        }

        if (!lua_isboolean(L, -1)) {
            lua_pop(L, 1);
            luaL_error(L, "configure: %s must be a boolean", name);
            return false;
        }

        target = lua_toboolean(L, -1);
        lua_pop(L, 1);
        return true;
    };

    if (!readInt("gesture_distance", g_luaConfig.gestureDistance) || !readFloat("scale", g_luaConfig.scale) ||
        !readInt("workspace_gap", g_luaConfig.workspaceGap) || !readInt("wallpaper", g_luaConfig.wallpaper) || !readBool("blur", g_luaConfig.blur))
        return 0;

    lua_getfield(L, TABLE, "shadow");
    if (lua_istable(L, -1)) {
        const int SHADOW = lua_absindex(L, -1);

        lua_getfield(L, SHADOW, "enabled");
        if (!lua_isnil(L, -1)) {
            if (!lua_isboolean(L, -1))
                return luaL_error(L, "configure: shadow.enabled must be a boolean");
            g_luaConfig.shadowEnabled = lua_toboolean(L, -1);
        }
        lua_pop(L, 1);

        lua_getfield(L, SHADOW, "range");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1))
                return luaL_error(L, "configure: shadow.range must be an integer");
            g_luaConfig.shadowRange = sc<int>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, SHADOW, "render_power");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1))
                return luaL_error(L, "configure: shadow.render_power must be an integer");
            g_luaConfig.shadowRenderPower = sc<int>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);

        lua_getfield(L, SHADOW, "color");
        if (!lua_isnil(L, -1)) {
            if (!lua_isinteger(L, -1))
                return luaL_error(L, "configure: shadow.color must be an integer");
            g_luaConfig.shadowColor = sc<int64_t>(lua_tointeger(L, -1));
        }
        lua_pop(L, 1);
    } else if (!lua_isnil(L, -1))
        return luaL_error(L, "configure: shadow must be a table");
    lua_pop(L, 1);

    return 0;
}

}

namespace ScrollOverview::Config {

const SScrollOverviewLuaConfig& lua() {
    return g_luaConfig;
}

void registerLua(TOverviewDispatcher dispatcher) {
    if (::Config::mgr()->type() != ::Config::CONFIG_LUA)
        return;

    g_overviewDispatcher = dispatcher;
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "overview", ::overviewLua);
    HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "configure", ::configureLua);
}

void registerLegacy() {
    // Registered through addConfigValueV2 (manager-agnostic) so the keys resolve
    // under both the legacy hyprland.conf and the Lua config managers — V1
    // addConfigValue is a no-op under the Lua manager. Read via ->value().
    g_soValues.gestureDistance   = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:gesture_distance", "Pixel distance treated as the gesture max", 200);
    g_soValues.scale             = makeShared<::Config::Values::CFloatValue>("plugin:scrolloverview:scale", "Overview workspace zoom factor", 0.5F);
    g_soValues.workspaceGap      = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:workspace_gap", "Pixel gap between workspaces in overview", 0);
    g_soValues.wallpaperMode     = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:wallpaper", "0 global only, 1 per-workspace only, 2 both", 0);
    // Optional image-file backdrop — needed for Quickshell/Qt-painted wallpaper
    // setups (dots-hyprland) whose layer surface isn't stable to sample.
    g_soValues.wallpaperPath     = makeShared<::Config::Values::CStringValue>("plugin:scrolloverview:wallpaper_path", "Path to a static image used as the overview backdrop", "");
    g_soValues.blur              = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:blur", "Blur the global overview wallpaper", 0);
    g_soValues.shadowEnabled     = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:shadow:enabled", "Render shadows behind overview windows", 0);
    g_soValues.shadowRange       = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:shadow:range", "Overview shadow range, -1 inherits", -1);
    g_soValues.shadowRenderPower = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:shadow:render_power", "Overview shadow render power, -1 inherits", -1);
    g_soValues.shadowColor       = makeShared<::Config::Values::CIntValue>("plugin:scrolloverview:shadow:color", "Overview shadow color, -1 inherits", -1);

    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.gestureDistance);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.scale);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.workspaceGap);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.wallpaperMode);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.wallpaperPath);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.blur);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.shadowEnabled);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.shadowRange);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.shadowRenderPower);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_soValues.shadowColor);
}

int getGestureDistance() {
    if (g_luaConfig.gestureDistance)
        return std::max<int>(1, *g_luaConfig.gestureDistance);
    return std::max<int>(1, g_soValues.gestureDistance ? sc<int>(g_soValues.gestureDistance->value()) : 200);
}

float getScale() {
    if (g_luaConfig.scale)
        return std::clamp(*g_luaConfig.scale, 0.1F, 0.9F);
    return std::clamp(g_soValues.scale ? sc<float>(g_soValues.scale->value()) : 0.5F, 0.1F, 0.9F);
}

int getWorkspaceGap() {
    if (g_luaConfig.workspaceGap)
        return std::max<int>(0, *g_luaConfig.workspaceGap);
    return std::max<int>(0, g_soValues.workspaceGap ? sc<int>(g_soValues.workspaceGap->value()) : 0);
}

int getWallpaperMode() {
    if (g_luaConfig.wallpaper)
        return std::clamp<int>(*g_luaConfig.wallpaper, 0, 2);
    return std::clamp<int>(g_soValues.wallpaperMode ? sc<int>(g_soValues.wallpaperMode->value()) : 0, 0, 2);
}

bool getBlur() {
    if (g_luaConfig.blur)
        return *g_luaConfig.blur;
    return g_soValues.blur && g_soValues.blur->value() != 0;
}

std::string getWallpaperPath() {
    if (g_soValues.wallpaperPath)
        return g_soValues.wallpaperPath->value();
    return "";
}

::Config::CCssGapData getCssGapData(const std::string& name) {
    const auto VALUE = HyprlandAPI::getConfigValue(SCROLLOVERVIEW_HANDLE, name);
    if (!VALUE)
        return {};

    const auto CUSTOM = (Hyprlang::CUSTOMTYPE* const*)(VALUE->getDataStaticPtr());
    if (!CUSTOM || !*CUSTOM)
        return {};

    const auto* const GAPS = static_cast<::Config::CCssGapData*>((*CUSTOM)->getData());
    return GAPS ? *GAPS : ::Config::CCssGapData{};
}

int getShadowEnabled() {
    if (g_luaConfig.shadowEnabled)
        return *g_luaConfig.shadowEnabled ? 1 : 0;
    return g_soValues.shadowEnabled ? sc<int>(g_soValues.shadowEnabled->value()) : 0;
}

int getShadowRange() {
    if (g_luaConfig.shadowRange)
        return *g_luaConfig.shadowRange;
    return g_soValues.shadowRange ? sc<int>(g_soValues.shadowRange->value()) : -1;
}

int getShadowRenderPower() {
    if (g_luaConfig.shadowRenderPower)
        return *g_luaConfig.shadowRenderPower;
    return g_soValues.shadowRenderPower ? sc<int>(g_soValues.shadowRenderPower->value()) : -1;
}

int64_t getShadowColor() {
    if (g_luaConfig.shadowColor)
        return *g_luaConfig.shadowColor;
    return g_soValues.shadowColor ? sc<int64_t>(g_soValues.shadowColor->value()) : -1;
}

}
