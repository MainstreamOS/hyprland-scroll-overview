#define WLR_USE_UNSTABLE

#include <unistd.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/managers/input/trackpad/GestureTypes.hpp>
#include <hyprland/src/managers/input/trackpad/TrackpadGestures.hpp>

#include <hyprutils/string/ConstVarList.hpp>
using namespace Hyprutils::String;

// Lua C API headers — extern "C" so name-mangling stays C-style. Used by
// luaOverviewDispatcher (the addLuaFunction wrapper exposed in Lua mode for
// the scrolloverview:overview dispatcher; plugin dispatchers with colons in
// their name can't be invoked from `hl.dispatch` so a Lua-callable wrapper
// is required for keybind/Quickshell access in 0.55+).
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "globals.hpp"
#include "scrollOverview.hpp"
#include "OverviewGesture.hpp"

// Methods
static CFunctionHook* g_pScrollRenderWorkspaceHook = nullptr;
static CFunctionHook* g_pScrollAddDamageHookA      = nullptr;
static CFunctionHook* g_pScrollAddDamageHookB      = nullptr;
static CFunctionHook* g_pScrollDamageSurfaceHook   = nullptr;
static CFunctionHook* g_pScrollScheduleFrameHook   = nullptr;
static CFunctionHook* g_pScrollSendFrameEventsHook = nullptr;
static CFunctionHook* g_pScrollSurfaceFrameHook    = nullptr;
typedef void (*origRenderWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&, const CBox&);
typedef void (*origAddDamageA)(void*, const CBox&);
typedef void (*origAddDamageB)(void*, const pixman_region32_t*);
typedef void (*origDamageSurface)(void*, SP<CWLSurfaceResource>, double, double, double);
typedef void (*origScheduleFrameForMonitor)(void*, PHLMONITOR, Aquamarine::IOutput::scheduleFrameReason);
typedef void (*origSendFrameEventsToWorkspace)(void*, PHLMONITOR, PHLWORKSPACE, const Time::steady_tp&);
typedef void (*origSurfaceFrame)(void*, const Time::steady_tp&);

static bool g_unloading = false;

// Do NOT change this function.
APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

static bool renderingOverview = false;
static bool damageFromSurface = false;
static bool g_scrollOverviewHooksActive = false;

static void failNotif(const std::string& reason);

bool ensureScrollOverviewHooks() {
    if (g_scrollOverviewHooksActive)
        return true;

    bool success = g_pScrollRenderWorkspaceHook->hook();
    success      = success && g_pScrollScheduleFrameHook->hook();
    success      = success && g_pScrollDamageSurfaceHook->hook();
    success      = success && g_pScrollSendFrameEventsHook->hook();
    success      = success && g_pScrollSurfaceFrameHook->hook();
    success      = success && g_pScrollAddDamageHookA->hook();
    success      = success && g_pScrollAddDamageHookB->hook();

    if (!success) {
        disableScrollOverviewHooks();
        failNotif("Failed enabling overview hooks");
        return false;
    }

    g_scrollOverviewHooksActive = true;
    return true;
}

void disableScrollOverviewHooks() {
    if (g_pScrollAddDamageHookB)
        g_pScrollAddDamageHookB->unhook();
    if (g_pScrollAddDamageHookA)
        g_pScrollAddDamageHookA->unhook();
    if (g_pScrollSurfaceFrameHook)
        g_pScrollSurfaceFrameHook->unhook();
    if (g_pScrollSendFrameEventsHook)
        g_pScrollSendFrameEventsHook->unhook();
    if (g_pScrollDamageSurfaceHook)
        g_pScrollDamageSurfaceHook->unhook();
    if (g_pScrollScheduleFrameHook)
        g_pScrollScheduleFrameHook->unhook();
    if (g_pScrollRenderWorkspaceHook)
        g_pScrollRenderWorkspaceHook->unhook();

    g_scrollOverviewHooksActive = false;
}

static void hkScheduleFrameForMonitor(void* thisptr, PHLMONITOR monitor, Aquamarine::IOutput::scheduleFrameReason reason) {
    if (g_pScrollOverview && g_pScrollOverview->pMonitor == monitor) {
        using enum Aquamarine::IOutput::scheduleFrameReason;

        const bool THROTTLEDREASON =
            reason == AQ_SCHEDULE_UNKNOWN || reason == AQ_SCHEDULE_CLIENT_UNKNOWN || reason == AQ_SCHEDULE_NEEDS_FRAME || reason == AQ_SCHEDULE_RENDER_MONITOR ||
            reason == AQ_SCHEDULE_DAMAGE;

        if (THROTTLEDREASON && !g_pScrollOverview->blockDamageReporting && !g_pScrollOverview->shouldAllowRealtimePreviewSchedule())
            return;
    }

    ((origScheduleFrameForMonitor)g_pScrollScheduleFrameHook->m_original)(thisptr, monitor, reason);
}

//
static void hkRenderWorkspace(void* thisptr, PHLMONITOR pMonitor, PHLWORKSPACE pWorkspace, const Time::steady_tp& now, const CBox& geometry) {
    if (!g_pScrollOverview || renderingOverview || g_pScrollOverview->blockOverviewRendering || g_pScrollOverview->pMonitor != pMonitor)
        ((origRenderWorkspace)(g_pScrollRenderWorkspaceHook->m_original))(thisptr, pMonitor, pWorkspace, now, geometry);
    else {
        const bool PREVRENDERINGOVERVIEW = renderingOverview;
        renderingOverview                = true;
        g_pScrollOverview->render();
        renderingOverview = PREVRENDERINGOVERVIEW;
    }
}

static void hkDamageSurface(void* thisptr, SP<CWLSurfaceResource> surface, double x, double y, double scale) {
    if (!g_pScrollOverview || g_pScrollOverview->blockDamageReporting || g_pScrollOverview->shouldHandleSurfaceDamage(surface)) {
        const bool PREVDAMAGEFROMSURFACE = damageFromSurface;
        damageFromSurface                = !!g_pScrollOverview;
        ((origDamageSurface)g_pScrollDamageSurfaceHook->m_original)(thisptr, surface, x, y, scale);
        damageFromSurface = PREVDAMAGEFROMSURFACE;
    }
}

static void hkSendFrameEventsToWorkspace(void* thisptr, PHLMONITOR monitor, PHLWORKSPACE workspace, const Time::steady_tp& now) {
    if (g_pScrollOverview && g_pScrollOverview->pMonitor == monitor)
        return;

    ((origSendFrameEventsToWorkspace)g_pScrollSendFrameEventsHook->m_original)(thisptr, monitor, workspace, now);
}

static void hkSurfaceFrame(void* thisptr, const Time::steady_tp& now) {
    const auto SURFACE = sc<CWLSurfaceResource*>(thisptr)->m_self.lock();

    if (g_pScrollOverview && !g_pScrollOverview->shouldAllowSurfaceFrame(SURFACE, now))
        return;

    ((origSurfaceFrame)g_pScrollSurfaceFrameHook->m_original)(thisptr, now);
}

static void hkAddDamageA(void* thisptr, const CBox& box) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (g_pScrollOverview && g_pScrollOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pScrollOverview->shouldSuppressRenderDamage()) {
        return;
    }

    if (!g_pScrollOverview || g_pScrollOverview->pMonitor != PMONITOR->m_self || g_pScrollOverview->blockDamageReporting || damageFromSurface) {
        ((origAddDamageA)g_pScrollAddDamageHookA->m_original)(thisptr, box);
        return;
    }

    g_pScrollOverview->onDamageReported();
    ((origAddDamageA)g_pScrollAddDamageHookA->m_original)(thisptr, box);
}

static void hkAddDamageB(void* thisptr, const pixman_region32_t* rg) {
    const auto PMONITOR = (CMonitor*)thisptr;

    if (g_pScrollOverview && g_pScrollOverview->pMonitor == PMONITOR->m_self && renderingOverview && !damageFromSurface && g_pScrollOverview->shouldSuppressRenderDamage()) {
        return;
    }

    if (!g_pScrollOverview || g_pScrollOverview->pMonitor != PMONITOR->m_self || g_pScrollOverview->blockDamageReporting || damageFromSurface) {
        ((origAddDamageB)g_pScrollAddDamageHookB->m_original)(thisptr, rg);
        return;
    }

    g_pScrollOverview->onDamageReported();
    ((origAddDamageB)g_pScrollAddDamageHookB->m_original)(thisptr, rg);
}

static SDispatchResult onOverviewDispatcher(std::string arg) {
    if (g_pScrollOverview && g_pScrollOverview->m_isSwiping)
        return {.success = false, .error = "already swiping"};

    if (arg == "select") {
        if (g_pScrollOverview) {
            g_pScrollOverview->selectHoveredWorkspace();
            g_pScrollOverview->close();
        }
        return {};
    }
    if (arg == "toggle") {
        if (g_pScrollOverview)
            g_pScrollOverview->close();
        else {
            if (!ensureScrollOverviewHooks())
                return {.success = false, .error = "failed enabling overview hooks"};

            renderingOverview = true;
            g_pScrollOverview = makeShared<CScrollOverview>(Desktop::focusState()->monitor()->m_activeWorkspace);
            renderingOverview = false;
        }
        return {};
    }

    if (arg == "off" || arg == "close" || arg == "disable") {
        if (g_pScrollOverview)
            g_pScrollOverview->close();
        return {};
    }

    if (g_pScrollOverview)
        return {};

    if (!ensureScrollOverviewHooks())
        return {.success = false, .error = "failed enabling overview hooks"};

    renderingOverview = true;
    g_pScrollOverview = makeShared<CScrollOverview>(Desktop::focusState()->monitor()->m_activeWorkspace);
    renderingOverview = false;
    return {};
}

static void failNotif(const std::string& reason) {
    HyprlandAPI::addNotification(SCROLLOVERVIEW_HANDLE, "[scrolloverview] Failure in initialization: " + reason, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
}

// Helper function to find a function by name and ensure it contains a specific substring in its demangled name (to disambiguate overloads).
static void* findFnOrThrow(const std::string& name, std::string_view mustContainDemangled) {
    auto fns = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name);
    if (fns.empty()) {
        failNotif(std::format("no fns for hook {}", name));
        throw std::runtime_error(std::format("[scrolloverview] No fns for hook {}", name));
    }

    if (mustContainDemangled.empty())
        return fns[0].address;

    std::vector<SFunctionMatch> matches;
    matches.reserve(fns.size());
    for (const auto& fn : fns) {
        if (fn.demangled.find(mustContainDemangled) != std::string::npos)
            matches.push_back(fn);
    }

    if (matches.empty()) {
        failNotif(std::format("no matching overload for hook {}", name));
        throw std::runtime_error(std::format("[scrolloverview] No matching overload for hook {}", name));
    }

    if (matches.size() > 1) {
        failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
        throw std::runtime_error(std::format("[scrolloverview] Ambiguous overload for hook {}", name));
    }

    return matches[0].address;
}

static Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;
    eTrackpadGestureDirection direction   = TRACKPAD_GESTURE_DIR_NONE;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    if (fingerCount <= 1 || fingerCount >= 10) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    direction = g_pTrackpadGestures->dirForString(data[1]);

    if (direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx = 2;
    uint32_t modMask      = 0;
    float    deltaScale   = 1.F;

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string{data[startDataIdx].substr(4)});
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string{data[startDataIdx].substr(6)}), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                result.setError(std::format("Invalid delta scale: {}", std::string{data[startDataIdx].substr(6)}).c_str());
                return result;
            }
        }

        break;
    }

    std::expected<void, std::string> resultFromGesture;

    if (data[startDataIdx] == "overview")
        resultFromGesture = g_pTrackpadGestures->addGesture(makeUnique<COverviewGesture>(), fingerCount, direction, modMask, deltaScale, false);
    else if (data[startDataIdx] == "unset")
        resultFromGesture = g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, false);
    else {
        result.setError(std::format("Invalid gesture: {}", data[startDataIdx]).c_str());
        return result;
    }

    if (!resultFromGesture) {
        result.setError(resultFromGesture.error().c_str());
        return result;
    }

    return result;
}

// Lua-callable wrapper around onOverviewDispatcher.
//
// In Lua mode, plugin dispatchers registered via addDispatcherV2 land in
// the C++ m_dispatchers map but are NOT exposed as Lua callables — and
// `hl.dispatch` only accepts hl.dsp.* userdata or Lua functions, never a
// string name. Plus the colon in "scrolloverview:overview" makes hyprctl's
// Lua-wrap (`return hl.dispatch(scrolloverview:overview ...)`) fail at the
// parser before we ever reach a callable.
//
// addLuaFunction registers this wrapper under hl.plugin.scrolloverview.overview,
// reachable from Lua as e.g.:
//   hl.bind("SUPER + O", function() hl.plugin.scrolloverview.overview("toggle") end)
// or from outside Hyprland via:
//   hyprctl eval 'hl.plugin.scrolloverview.overview("on")'
//
// The dispatcher accepts the same arg strings as the C++ dispatcher
// ("toggle", "select", "off"/"close"/"disable", or default = open).
static int luaOverviewDispatcher(lua_State* L) {
    const char* arg = luaL_optstring(L, 1, "toggle");
    ::onOverviewDispatcher(std::string(arg));
    return 0;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    SCROLLOVERVIEW_HANDLE = handle;

    const std::string HASH        = __hyprland_api_get_hash();
    const std::string CLIENT_HASH = __hyprland_api_get_client_hash();

    if (HASH != CLIENT_HASH) {
        failNotif("Version mismatch (headers ver is not equal to running hyprland ver)");
        throw std::runtime_error("[he] Version mismatch");
    }

    // 0.55: CHyprRenderer was renamed to Render::IHyprRenderer (the
    // renderer base class). The mangled symbols all carry the new
    // namespace+class prefix. CCompositor::scheduleFrameForMonitor stayed
    // put.
    g_pScrollRenderWorkspaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("renderWorkspace", "Render::IHyprRenderer::renderWorkspace("),
        (void*)hkRenderWorkspace);

    g_pScrollScheduleFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("scheduleFrameForMonitor", "CCompositor::scheduleFrameForMonitor("),
        (void*)hkScheduleFrameForMonitor);

    g_pScrollDamageSurfaceHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("damageSurface", "Render::IHyprRenderer::damageSurface("),
        (void*)hkDamageSurface);

    g_pScrollSendFrameEventsHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("sendFrameEventsToWorkspace", "Render::IHyprRenderer::sendFrameEventsToWorkspace("),
        (void*)hkSendFrameEventsToWorkspace);

    g_pScrollSurfaceFrameHook = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE", ""),
        (void*)hkSurfaceFrame);

    g_pScrollAddDamageHookB = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("addDamageEPK15pixman_region32", "CMonitor::addDamage"),
        (void*)hkAddDamageB);

    g_pScrollAddDamageHookA = HyprlandAPI::createFunctionHook(
        SCROLLOVERVIEW_HANDLE,
        findFnOrThrow("_ZN8CMonitor9addDamageERKN9Hyprutils4Math4CBoxE", ""),
        (void*)hkAddDamageA);

    static auto P = Event::bus()->m_events.render.pre.listen([](PHLMONITOR monitor) {
        if (!g_pScrollOverview || g_pScrollOverview->pMonitor != monitor)
            return;
        g_pScrollOverview->onPreRender();
    });

    HyprlandAPI::addDispatcherV2(SCROLLOVERVIEW_HANDLE, "scrolloverview:overview", ::onOverviewDispatcher);

    // Lua-mode exposure for the overview dispatcher (see luaOverviewDispatcher
    // comment above). addLuaFunction is no-op outside Lua mode (PluginAPI.cpp:470).
    if (Config::mgr()->type() == Config::CONFIG_LUA)
        HyprlandAPI::addLuaFunction(SCROLLOVERVIEW_HANDLE, "scrolloverview", "overview", ::luaOverviewDispatcher);

    // scrolloverview-gesture is only usable in Legacy mode (addConfigKeyword
    // is gated on CONFIG_LEGACY in PluginAPI.cpp:198). Skipping it under Lua
    // costs nothing since none of the dots-hyprland configs use it; users on
    // pure-hyprlang setups still get the keyword.
    if (Config::mgr()->type() == Config::CONFIG_LEGACY)
        HyprlandAPI::addConfigKeyword(SCROLLOVERVIEW_HANDLE, "scrolloverview-gesture", ::overviewGestureKeyword, {});

    // V2 config registration. addConfigValue (V1) is a no-op when Hyprland
    // runs the Lua config manager (PluginAPI.cpp:179 gates on CONFIG_LEGACY),
    // so we use addConfigValueV2 + IValue subclasses to register through the
    // manager-agnostic path. The scrolloverview-gesture custom keyword was
    // dropped — it wasn't referenced by any shipped config and the V1
    // addConfigKeyword is also Legacy-only.
    g_pSOConfig = makeUnique<SScrollOverviewConfig>();

    g_pSOConfig->gestureDistance =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:gesture_distance", "Pixel distance treated as the gesture max", 200);
    g_pSOConfig->scale =
        makeShared<Config::Values::CFloatValue>("plugin:scrolloverview:scale", "Overview workspace zoom factor", 0.5F);
    g_pSOConfig->workspaceGap =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:workspace_gap", "Pixel gap between workspaces in overview", 100);
    g_pSOConfig->wallpaperMode =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:wallpaper", "0 global only, 1 per-workspace only, 2 both", 0);
    // Optional path to an image file used as the global wallpaper. When set,
    // the backdrop is rendered from this file instead of by iterating
    // ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND surfaces. Useful for setups
    // (end-4 / Quickshell-painted) where the wlr-layer-shell wallpaper
    // surface texture isn't stable to capture mid-render.
    g_pSOConfig->wallpaperPath =
        makeShared<Config::Values::CStringValue>("plugin:scrolloverview:wallpaper_path", "Path to a static image to use as the overview backdrop", "");
    g_pSOConfig->blur =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:blur", "Blur the global overview wallpaper", 0);
    // Open and close speeds are both hardcoded in scrollOverview.cpp;
    // the bezier is also fixed (cubic ease-in-out). No user-facing
    // config to tune the open/close animation.
    g_pSOConfig->shadowEnabled =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:shadow:enabled", "Render shadows behind overview windows", 0);
    g_pSOConfig->shadowRange =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:shadow:range", "Overview shadow range, -1 inherits decoration:shadow:range", -1);
    g_pSOConfig->shadowRenderPower =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:shadow:render_power", "Overview shadow render power, -1 inherits", -1);
    g_pSOConfig->shadowIgnoreWindow =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:shadow:ignore_window", "Overview shadow ignore window, -1 inherits", -1);
    g_pSOConfig->shadowColor =
        makeShared<Config::Values::CIntValue>("plugin:scrolloverview:shadow:color", "Overview shadow color, -1 inherits decoration:shadow:color", -1);

    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->gestureDistance);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->scale);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->workspaceGap);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->wallpaperMode);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->wallpaperPath);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->blur);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->shadowEnabled);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->shadowRange);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->shadowRenderPower);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->shadowIgnoreWindow);
    HyprlandAPI::addConfigValueV2(SCROLLOVERVIEW_HANDLE, g_pSOConfig->shadowColor);

    HyprlandAPI::reloadConfig();

    return {"scrolloverview", "A plugin for an overview", "Vaxry, yayuuu", "1.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CScrollOverviewPassElement");

    g_unloading = true;
    g_pScrollOverview.reset();
    disableScrollOverviewHooks();

    // Drop the V2 IValue ownership. addConfigValueV2 also keeps a weak
    // reference inside m_registeredApiValues that Hyprland clears on
    // plugin unload, but releasing our SPs is what actually triggers
    // destruction of the value objects.
    g_pSOConfig.reset();

    HyprlandAPI::reloadConfig(); // 0.55: g_pConfigManager removed; HyprlandAPI exposes the reload entry point. We need to reload now to clear all the gestures.
}
