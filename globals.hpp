#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

#include <string>

inline HANDLE SCROLLOVERVIEW_HANDLE = nullptr;

bool ensureScrollOverviewHooks();
void disableScrollOverviewHooks();

// V2 plugin-API port (Hyprland 0.55+).
//
// HyprlandAPI::{addConfigValue,addConfigKeyword,getConfigValue} are hard-gated
// to CONFIG_LEGACY in src/plugins/PluginAPI.cpp:179,198,214 — they no-op when
// the Lua manager is active. To stay functional under either manager we:
//
//   1. Own our config keys as V2 IValue objects (kept alive in g_pSOConfig)
//      and register them via HyprlandAPI::addConfigValueV2.
//   2. Read everything (our own keys + Hyprland builtins) through the
//      manager-agnostic Config::mgr()->getConfigValue() virtual, wrapped by
//      the soConfigPtr<T>() helper below.
//
// Lookup keeps the same hot-path shape as the old V1 static-cached pointer
// pattern — one lookup per call site, dereferenced on each use.
struct SScrollOverviewConfig {
    SP<Config::Values::CIntValue>    gestureDistance;
    SP<Config::Values::CFloatValue>  scale;
    SP<Config::Values::CIntValue>    workspaceGap;
    SP<Config::Values::CIntValue>    wallpaperMode;
    SP<Config::Values::CStringValue> wallpaperPath;
    SP<Config::Values::CIntValue>    blur;
    SP<Config::Values::CIntValue>    shadowEnabled;
    SP<Config::Values::CIntValue>    shadowRange;
    SP<Config::Values::CIntValue>    shadowRenderPower;
    SP<Config::Values::CIntValue>    shadowIgnoreWindow;
    SP<Config::Values::CIntValue>    shadowColor;
};

inline UP<SScrollOverviewConfig> g_pSOConfig;

// Read a config value's data pointer through the active config manager (Lua
// or Legacy). Both managers populate SConfigOptionReply.dataptr as a
// `void* const*` pointing at the value's storage. We reinterpret as `T*`
// (one level of indirection — getConfigValue gives back &storage_ptr; the
// inner pointer is the value itself).
//
// Returns nullptr when the key isn't registered (e.g. a builtin removed by
// upstream, or a plugin key our PLUGIN_INIT failed to register). Callers
// must null-check before dereferencing.
template <typename T>
inline T* soConfigPtr(const std::string& name) {
    const auto reply = Config::mgr()->getConfigValue(name);
    if (!reply.dataptr)
        return nullptr;
    return *reinterpret_cast<T* const*>(reply.dataptr);
}
