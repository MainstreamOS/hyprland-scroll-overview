#pragma once

#define WLR_USE_UNSTABLE

#include "globals.hpp"
#include <hyprland/src/desktop/DesktopTypes.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/render/Framebuffer.hpp>           // 0.55: defines Render::IFramebuffer
#include <hyprland/src/render/gl/GLFramebuffer.hpp>      // 0.55: defines Render::CGLFramebuffer (the GL impl of IFramebuffer); we hold the backdrop blur cache as SP<CGLFramebuffer>
#include <chrono>
#include <unordered_map>
#include <vector>

#include "IOverview.hpp"

class CMonitor;
// 0.55: CTexture was split into Render::ITexture (interface) and
// Render::GL::CGLTexture (the GL impl). Hold textures as the interface
// type — every renderer entry point that takes a texture takes SP<ITexture>.
namespace Render {
    class ITexture;
}
struct wl_event_source;

class CScrollOverview : public IOverview {
  public:
    CScrollOverview(PHLWORKSPACE startedOn_, bool swipe = false);
    virtual ~CScrollOverview();

    virtual void render();
    virtual void damage();
    void         markBlurDirty();
    void         markBackdropBlurDirty();
    virtual void onDamageReported();
    virtual bool shouldHandleSurfaceDamage(SP<CWLSurfaceResource> surface);
    virtual bool shouldAllowSurfaceFrame(SP<CWLSurfaceResource> surface, const Time::steady_tp& now);
    virtual bool shouldAllowRealtimePreviewSchedule();
    virtual bool shouldSuppressRenderDamage() const;
    virtual void onPreRender();

    virtual void setClosing(bool closing);

    virtual void resetSwipe();
    virtual void onSwipeUpdate(double delta);
    virtual void onSwipeEnd();

    // close without a selection
    virtual void close();
    virtual void selectHoveredWorkspace();

    virtual void fullRender();

  private:
    void   rebuildWorkspaceImages();
    void   seedRememberedSelections();
    void   redrawAll(bool forcelowres = false);
    void   onWorkspaceChange();
    void   renderGlobalWallpaper(PHLMONITOR monitor, const Time::steady_tp& now);
    void   renderWallpaperLayers(PHLMONITOR monitor, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F);
    void   updateBackdropBlurCache(PHLMONITOR monitor, int wallpaperMode, const Time::steady_tp& now);
    void   renderBackdropBlurCache(PHLMONITOR monitor);
    void   renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    void   renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now);
    bool   hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const;
    void   renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox = nullptr,
                             bool usePrecomputedBlur = false);
    void   renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now);
    void   renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now);
    void   moveViewportWorkspace(bool up);
    bool   moveWindowSelection(const std::string& direction);
    void   rememberSelection(PHLWINDOW window);
    void   syncSelectionToViewport();
    void   syncFocusedSelection();
    float      workspaceOverviewYOffset(size_t workspaceIdx, size_t activeIdx, float workspacePitch) const;
    float      workspaceOverviewAlpha(size_t workspaceIdx) const;
    PHLWINDOW windowAtOverviewCursor(size_t* workspaceIdx = nullptr);
    PHLWINDOW windowAtOverviewCursorOnWorkspace(size_t workspaceIdx, const PHLWINDOW& ignoredWindow = nullptr, CBox* windowBox = nullptr) const;
    PHLWORKSPACE workspaceAtOverviewCursor(size_t* workspaceIdx = nullptr) const;
    Vector2D  overviewPointToGlobal(size_t workspaceIdx, const Vector2D& pointLocal) const;
    CBox      draggedWindowBox(size_t workspaceIdx) const;
    void      beginWindowDrag();
    void      updateWindowDrag();
    void      endWindowDrag();
    CBox      resizedWindowBox() const;
    void      beginWindowResize();
    void      updateWindowResize();
    void      endWindowResize();
    void   forceSurfaceVisibility(SP<CWLSurfaceResource> surface);
    void   forceWindowSurfaceVisibility(PHLWINDOW window);
    void   forceWindowVisible(PHLWINDOW window);
    void   forceLayersAboveFullscreen();
    void   restoreForcedSurfaceVisibility();
    void   restoreForcedWindowVisibility();
    void   restoreForcedLayerVisibility();
    void   emitFullscreenVisibilityState(PHLWINDOW window, bool hideFullscreen);
    void   applyInputConfigOverrides();
    void   restoreInputConfigOverrides();
    size_t activeWorkspaceIndex() const;
    void   sendOverviewFrameCallbacks(const Time::steady_tp& now);
    bool   isVisibleRealtimePreviewWindow(const PHLWINDOW& window) const;
    bool   shouldAllowRealtimePreviewFrame() const;
    void   scheduleMinimumPreviewFrame();
    void   schedulePreviewFrameAfter(std::chrono::milliseconds delay);
    void   scheduleRealtimePreviewFrame();
    static int realtimePreviewTimerCallback(void* data);

    size_t viewportCurrentWorkspace = 0;
    bool   rebuildPending           = false;
    bool   workspaceSyncPending     = false;
    bool   overviewBlurDirty        = true;
    bool   backdropBlurDirty        = true;
    bool   overviewBlurStateValid   = false;
    float  lastOverviewBlurScale    = 1.F;
    int    lastBackdropWallpaperMode = -1;
    Vector2D lastOverviewBlurViewOffset = Vector2D{};
    // 0.55: CFramebuffer renamed to Render::CGLFramebuffer; m_renderData.currentFB
    // became SP<IFramebuffer>, so we hold this as a shared_ptr now too — assignment
    // to currentFB then works as a normal SP-to-SP-of-base copy without an
    // address-of trick.
    SP<Render::GL::CGLFramebuffer> backdropBlurFB;

    struct SWorkspaceImage {
        PHLWORKSPACE              pWorkspace;
        std::vector<PHLWINDOWREF> windows;
    };

    struct SWorkspaceInsertTransition {
        bool                             active              = false;
        WORKSPACEID                      transitionWorkspaceID = WORKSPACE_INVALID;
        bool                             transitionFadeIn   = true;
        std::unordered_map<WORKSPACEID, long> oldRelativeSlots;
        std::unordered_map<WORKSPACEID, long> newRelativeSlots;
        long                             transitionOldRelativeSlot = 0;
    };

    Vector2D                         lastMousePosLocal = Vector2D{}; // monitor-local pixel space

    PHLWINDOWREF                     closeOnWindow;
    PHLWINDOWREF                     dragPendingWindow;
    PHLWINDOWREF                     dragActiveWindow;
    PHLWORKSPACEREF                  dragOriginalWorkspace;
    PHLWINDOWREF                     resizePendingWindow;
    PHLWINDOWREF                     resizeActiveWindow;

    Vector2D                         dragStartMouseLocal   = Vector2D{};
    Vector2D                         dragOriginalFloatSize = Vector2D{};
    Vector2D                         resizeStartMouseLocal = Vector2D{};
    Vector2D                         resizeLastMouseLocal  = Vector2D{};
    CBox                             dragOriginalBox        = CBox{};
    CBox                             resizeOriginalBox      = CBox{};
    size_t                           resizeWorkspaceIdx     = 0;
    Layout::eRectCorner              resizeCorner           = Layout::CORNER_NONE;
    bool                             dragPointerDown       = false;
    bool                             resizePointerDown     = false;
    bool                             dragStartedTiled      = false;
    bool                             emittingFullscreenVisibilityState = false;
    bool                             inputConfigOverridden = false;
    bool                             realtimePreviewTimerArmed = false;
    bool                             realtimePreviewFrameQueued = false;
    bool                             sendingOverviewFrameCallbacks = false;
    int                              previousNoWarps = 0;
    int                              previousWarpOnChangeWorkspace = 0;
    int                              previousWarpOnToggleSpecial = 0;
    int                              previousWarpBackAfterNonMouseInput = 0;
    int                              previousFollowMouse = 0;

    std::vector<SP<SWorkspaceImage>> images;
    std::vector<PHLWINDOWREF>        pinnedFloatingWindows;
    std::unordered_map<WORKSPACEID, PHLWINDOWREF> rememberedSelection;
    SWorkspaceInsertTransition       workspaceInsertTransition;
    PHLWORKSPACEREF                  pendingRemovedWorkspace;

    struct SForcedSurfaceVisibility {
        WP<CWLSurfaceResource> surface;
        CRegion               visibleRegion;
    };
    std::vector<SForcedSurfaceVisibility> forcedSurfaceVisibility;

    struct SForcedWindowVisibility {
        PHLWINDOWREF window;
        bool         hidden = false;
    };
    std::vector<SForcedWindowVisibility> forcedWindowVisibility;

    struct SForcedLayerVisibility {
        PHLLSREF layer;
        bool     aboveFullscreen = true;
        float    alpha           = 1.F;
    };
    std::vector<SForcedLayerVisibility> forcedLayerVisibility;

    PHLWORKSPACE                     startedOn;

    PHLANIMVAR<float>                scale;
    PHLANIMVAR<Vector2D>             viewOffset;
    PHLANIMVAR<float>                workspaceInsertProgress;
    PHLANIMVAR<float>                workspaceInsertFadeProgress;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceInsertFadeConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> workspaceRemoveFadeConfig;
    // Drive the overview zoom on `scale`. close() swaps `scale`'s active
    // config to overviewCloseConfig before kicking the close animation,
    // so we can have a slower opening (overviewOpenConfig) and a faster
    // closing (overviewCloseConfig) on the same animated variable. Both
    // are kept as members because the animation manager stores a weak
    // reference — if the SP drops, the animation degenerates to an
    // instant snap.
    SP<Hyprutils::Animation::SAnimationPropertyConfig> overviewOpenConfig;
    SP<Hyprutils::Animation::SAnimationPropertyConfig> overviewCloseConfig;
    Time::steady_tp                  lastRealtimePreviewFrame = {};
    Time::steady_tp                  realtimePreviewTimerDue = {};
    wl_event_source*                 realtimePreviewTimer = nullptr;

    bool                             closing = false;

    CHyprSignalListener             mouseMoveHook;
    CHyprSignalListener             mouseButtonHook;
    CHyprSignalListener             touchMoveHook;
    CHyprSignalListener             touchDownHook;
    CHyprSignalListener             mouseAxisHook;
    CHyprSignalListener             windowOpenHook;
    CHyprSignalListener             windowCloseHook;
    CHyprSignalListener             windowMoveHook;
    CHyprSignalListener             windowActiveHook;
    CHyprSignalListener             windowFullscreenHook;
    CHyprSignalListener             keyboardKeyHook;
    CHyprSignalListener             workspaceCreatedHook;
    CHyprSignalListener             workspaceRemovedHook;
    CHyprSignalListener             workspaceActiveHook;

    bool                             swipe = false;

    // Custom-wallpaper backdrop (see plugin:scrolloverview:wallpaper_path).
    // Loaded lazily by renderGlobalWallpaper(). Cached until the path config
    // changes, at which point both textures are dropped and reloaded.
    // m_customWallpaperBlurredTex holds a CPU-pre-blurred copy used when
    // plugin:scrolloverview:blur is enabled — replaces the GL blur path,
    // which doesn't function once the cache FB is the blur source (multiple
    // attempts via the various Hyprland blur APIs all produced GPU noise).
    SP<Render::ITexture>m_customWallpaperTex;
    SP<Render::ITexture>m_customWallpaperBlurredTex;
    std::string                      m_lastLoadedWallpaperPath;

    friend class CScrollOverviewPassElement;
};
