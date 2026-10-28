#include "globals.hpp"
#include "OffscreenCapture.hpp"

#include <algorithm>
#include <type_traits>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/protocols/types/Buffer.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#define private public
#include <hyprland/src/managers/screenshare/ScreenshareManager.hpp>
#undef private

static CFunctionHook* g_pOutputCommitHook = nullptr;
using origOutputCommit                    = void (*)(Screenshare::CScreenshareManager*, PHLMONITOR, bool);
static_assert(std::is_same_v<decltype(&Screenshare::CScreenshareManager::onOutputCommit),
                             void (Screenshare::CScreenshareManager::*)(PHLMONITOR, bool)>);

static void hkOutputCommit(Screenshare::CScreenshareManager* manager, PHLMONITOR monitor, bool copyFBPrepared) {
    rc<origOutputCommit>(g_pOutputCommitHook->m_original)(manager, monitor, copyFBPrepared);

    if (!copyFBPrepared || !monitor || manager->m_pendingFrames.empty())
        return;

    // Hyprland 0.56 skips window captures whose geometry is outside the monitor,
    // although renderWindow renders them independently with ignorePosition.
    // Finish only those skipped requests through the normal copy path, which
    // still enforces capture permissions and handles asynchronous GPU copies.
    // Snapshot the weak references because copy callbacks may retire requests.
    const auto FRAMES = manager->m_pendingFrames;
    for (const auto& weakFrame : FRAMES) {
        const auto FRAME = weakFrame;
        if (FRAME.expired() || !FRAME->m_shared || FRAME->done())
            continue;

        // Frames and sessions have unique owners. Keep non-owning references;
        // Hyprutils cannot promote these with lock().
        const auto SESSION = FRAME->m_session;
        if (SESSION.expired() || SESSION->m_stopped || SESSION->m_type != Screenshare::SHARE_WINDOW || SESSION->monitor() != monitor)
            continue;

        const auto WINDOW = SESSION->m_window;
        if (WINDOW.expired() || !WINDOW->mapped())
            continue;

        const auto GEOMETRY = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
        if (GEOMETRY.intersection({monitor->m_position, monitor->m_size}).empty())
            FRAME->copy();
    }

    std::erase_if(manager->m_pendingFrames, [](const auto& frame) { return frame.expired() || frame->done(); });
}

bool enableOffscreenCapture() {
    const auto FNS     = HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, "onOutputCommit");
    void*      address = nullptr;
    for (const auto& fn : FNS) {
        if (fn.demangled.find("Screenshare::CScreenshareManager::onOutputCommit(") == std::string::npos)
            continue;
        if (address)
            return false;
        address = fn.address;
    }

    if (!address)
        return false;

    g_pOutputCommitHook = HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, address, rc<void*>(hkOutputCommit));
    return g_pOutputCommitHook && g_pOutputCommitHook->hook();
}

void disableOffscreenCapture() {
    if (g_pOutputCommitHook)
        g_pOutputCommitHook->unhook();
}
