#include "globals.hpp"
#include "ScrollingCompatibility.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>

using namespace Layout::Tiled;

static CFunctionHook* g_pScrollingRecalculateHook = nullptr;
static CFunctionHook* g_pScrollingDataHook        = nullptr;
static CFunctionHook* g_pScrollingFocusHook       = nullptr;
using origRecalculate = void (*)(CScrollingAlgorithm*, Layout::eRecalculateReason);
using origData        = void (*)(SScrollingData*, bool);
using origFocus       = void (*)(CScrollingAlgorithm*, SP<Layout::ITarget>, CScrollingAlgorithm::eInputMode);

struct SViewport {
    WP<SScrollingData> data;
    double            extent;
    eScrollDirection  direction;
};
static std::unordered_map<CScrollingAlgorithm*, SViewport> g_viewports;

static void preserveViewport(SScrollingData* data) {
    if (!data || !data->algorithm || !data->controller)
        return;

    std::erase_if(g_viewports, [](const auto& entry) { return entry.second.data.expired(); });
    auto&      controller = *data->controller;
    const auto AREA       = data->algorithm->usableArea();
    const auto DIRECTION  = controller.getDirection();
    const auto EXTENT     = controller.isPrimaryHorizontal() ? AREA.w : AREA.h;
    if (!std::isfinite(EXTENT) || EXTENT <= 0)
        return;

    const auto PREVIOUS = g_viewports.find(data->algorithm);
    if (PREVIOUS != g_viewports.end() && PREVIOUS->second.data.get() == data && PREVIOUS->second.direction == DIRECTION) {
        const auto OLD_EXTENT = PREVIOUS->second.extent;
        if (OLD_EXTENT > 0 && OLD_EXTENT != EXTENT) {
            // Column widths follow the work area, but Hyprland 0.56 keeps the
            // camera offset in old pixels when a panel reserves an edge.
            // Preserve the position on the tape, including a partial column.
            const auto RATIO = EXTENT / OLD_EXTENT;
            if (controller.getScrollInhibitor().isInhibited)
                controller.getScrollInhibitor().offsetWhenInhibited *= RATIO;
            controller.setOffset(controller.getOffset() * RATIO);
        }
    }
    g_viewports.insert_or_assign(data->algorithm, SViewport{data->self, EXTENT, DIRECTION});
}

static void hkRecalculate(CScrollingAlgorithm* algorithm, Layout::eRecalculateReason reason) {
    const auto PREVIOUS = g_viewports.find(algorithm);
    if (PREVIOUS != g_viewports.end() && !PREVIOUS->second.data.expired())
        preserveViewport(PREVIOUS->second.data.get());
    rc<origRecalculate>(g_pScrollingRecalculateHook->m_original)(algorithm, reason);
}

static void hkData(SScrollingData* data, bool forceInstant) {
    preserveViewport(data);
    rc<origData>(g_pScrollingDataHook->m_original)(data, forceInstant);
}

static void hkFocus(CScrollingAlgorithm* algorithm, SP<Layout::ITarget> target, CScrollingAlgorithm::eInputMode input) {
    if (input == CScrollingAlgorithm::INPUT_MODE_CLICK) {
        const auto SURFACE = Desktop::View::CWLSurface::fromResource(g_pSeatManager->m_state.pointerFocus.lock());
        // The scrolling mouse-release listener also runs for panel clicks.
        // The last focused window can lie under that panel, so geometry alone
        // does not establish that it received the click.
        if (!SURFACE || !SURFACE->view() || SURFACE->view()->type() != Desktop::View::VIEW_TYPE_WINDOW)
            return;
    }
    rc<origFocus>(g_pScrollingFocusHook->m_original)(algorithm, target, input);
}

static CFunctionHook* createHook(const std::string& name, const std::string& signature, void* replacement) {
    void* address = nullptr;
    for (const auto& fn : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name)) {
        if (!fn.demangled.starts_with(signature))
            continue;
        if (address)
            return nullptr;
        address = fn.address;
    }
    return address ? HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, address, replacement) : nullptr;
}

bool enableScrollingCompatibility() {
    g_pScrollingRecalculateHook = createHook("recalculate", "Layout::Tiled::CScrollingAlgorithm::recalculate(", rc<void*>(hkRecalculate));
    g_pScrollingDataHook        = createHook("recalculate", "Layout::Tiled::SScrollingData::recalculate(", rc<void*>(hkData));
    g_pScrollingFocusHook       = createHook("focusOnInput", "Layout::Tiled::CScrollingAlgorithm::focusOnInput(", rc<void*>(hkFocus));
    if (!g_pScrollingRecalculateHook || !g_pScrollingDataHook || !g_pScrollingFocusHook
        || !g_pScrollingRecalculateHook->hook() || !g_pScrollingDataHook->hook() || !g_pScrollingFocusHook->hook()) {
        disableScrollingCompatibility();
        return false;
    }

    // Seed layouts that already existed when the plugin was loaded. Later
    // layouts register through their first data recalculation.
    for (const auto& window : Desktop::windowState()->windows()) {
        const auto TARGET = window->layoutTarget();
        const auto SPACE  = TARGET ? TARGET->space() : nullptr;
        const auto ALGO   = SPACE && SPACE->algorithm() ? dynamic_cast<CScrollingAlgorithm*>(SPACE->algorithm()->tiledAlgo().get()) : nullptr;
        const auto DATA   = ALGO ? ALGO->dataFor(TARGET) : nullptr;
        if (DATA && !DATA->column.expired() && !DATA->column->scrollingData.expired())
            preserveViewport(DATA->column->scrollingData.get());
    }
    return true;
}

void disableScrollingCompatibility() {
    if (g_pScrollingFocusHook)
        g_pScrollingFocusHook->unhook();
    if (g_pScrollingDataHook)
        g_pScrollingDataHook->unhook();
    if (g_pScrollingRecalculateHook)
        g_pScrollingRecalculateHook->unhook();
    g_viewports.clear();
}
