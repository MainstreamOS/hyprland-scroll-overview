#include "globals.hpp"
#include "ScrollingCompatibility.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingFullscreenHandler.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/managers/SeatManager.hpp>

using namespace Layout::Tiled;

static CFunctionHook* g_pScrollingRecalculateHook = nullptr;
static CFunctionHook* g_pScrollingDataHook        = nullptr;
static CFunctionHook* g_pScrollingFocusHook       = nullptr;
static CFunctionHook* g_pScrollingFullscreenHook  = nullptr;
using origRecalculate = void (*)(CScrollingAlgorithm*, Layout::eRecalculateReason);
using origData        = void (*)(SScrollingData*, bool);
using origFocus       = void (*)(CScrollingAlgorithm*, SP<Layout::ITarget>, CScrollingAlgorithm::eInputMode);
using origFullscreen  = Fullscreen::eFullscreenRequestResult (*)(Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler*, const Fullscreen::SFullscreenRequest&);

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

struct SRestoreState {
    WP<Layout::ITarget>                                target;
    WP<SScrollingData>                                 tape;
    WP<SColumnData>                                    column;
    WP<SColumnData>                                    fullscreenColumn;
    eScrollDirection                                   direction;
    double                                             offset;
    std::vector<std::pair<WP<Layout::ITarget>, float>> members;
};
static std::unordered_map<Layout::ITarget*, SRestoreState> g_restoreStates;

static Fullscreen::eFullscreenRequestResult hkFullscreen(Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler* handler,
                                                         const Fullscreen::SFullscreenRequest&                                request) {
    std::erase_if(g_restoreStates, [](const auto& entry) { return entry.second.target.expired() || entry.second.tape.expired(); });
    const auto SPACE = request.target ? request.target->space() : nullptr;
    const auto ALGO  = SPACE && SPACE->algorithm() ? dynamic_cast<CScrollingAlgorithm*>(SPACE->algorithm()->tiledAlgo().get()) : nullptr;
    const auto DATA  = ALGO ? ALGO->dataFor(request.target, true) : nullptr;
    const auto COL   = DATA ? DATA->column.lock() : nullptr;
    const auto TAPE  = COL ? COL->scrollingData.lock() : nullptr;
    std::optional<SRestoreState> saved;
    if (request.currentMode == Fullscreen::FSMODE_NONE)
        g_restoreStates.erase(request.target.get());
    if (TAPE && TAPE->controller && request.currentMode == Fullscreen::FSMODE_NONE && request.mode != Fullscreen::FSMODE_NONE) {
        const auto AREA   = ALGO->usableArea();
        const auto EXTENT = TAPE->controller->isPrimaryHorizontal() ? AREA.w : AREA.h;
        if (std::isfinite(EXTENT) && EXTENT > 0 && !TAPE->controller->getScrollInhibitor().isInhibited) {
            saved = SRestoreState{request.target, TAPE, COL, {}, TAPE->controller->getDirection(), TAPE->controller->getOffset() / EXTENT, {}};
            for (size_t i = 0; i < COL->targetDatas.size(); ++i)
                saved->members.emplace_back(COL->targetDatas[i]->target, COL->getTargetSize(i));
        }
    }

    const auto RESULT = rc<origFullscreen>(g_pScrollingFullscreenHook->m_original)(handler, request);
    if (RESULT != Fullscreen::FULLSCREEN_REQUEST_LAYOUT_HANDLED)
        return RESULT;
    if (saved) {
        saved->fullscreenColumn = DATA->column;
        g_restoreStates.insert_or_assign(request.target.get(), std::move(*saved));
    }
    if (request.mode != Fullscreen::FSMODE_NONE || request.currentMode == Fullscreen::FSMODE_NONE)
        return RESULT;

    const auto PREVIOUS = g_restoreStates.find(request.target.get());
    if (PREVIOUS != g_restoreStates.end()) {
        saved = std::move(PREVIOUS->second);
        g_restoreStates.erase(PREVIOUS);
    }
    if (!TAPE || !TAPE->controller || TAPE->controller->getScrollInhibitor().isInhibited)
        return RESULT;

    auto&      controller = *TAPE->controller;
    const auto AREA       = ALGO->usableArea();
    const auto EXTENT     = controller.isPrimaryHorizontal() ? AREA.w : AREA.h;
    if (!std::isfinite(EXTENT) || EXTENT <= 0)
        return RESULT;

    if (saved && saved->tape == TAPE && saved->direction == controller.getDirection() && saved->fullscreenColumn == DATA->column) {
        const auto ORIGINAL  = saved->column.lock();
        const auto CURRENT   = DATA->column.lock();
        bool       unchanged = ORIGINAL && CURRENT && TAPE->idx(ORIGINAL) >= 0;
        // Fullscreen expels a stacked window. Return it only if the remaining
        // stack is intact; explicit moves or edits made while fullscreen win.
        size_t sibling = 0;
        for (const auto& [target, size] : saved->members) {
            if (target == request.target)
                continue;
            if (!ORIGINAL || sibling >= ORIGINAL->targetDatas.size() || ORIGINAL->targetDatas[sibling++]->target != target)
                unchanged = false;
        }
        if (ORIGINAL != CURRENT)
            unchanged = unchanged && ORIGINAL->targetDatas.size() + 1 == saved->members.size() && CURRENT->targetDatas.size() == 1;
        else
            unchanged = unchanged && CURRENT->targetDatas.size() == saved->members.size();
        if (unchanged) {
            if (ORIGINAL != CURRENT) {
                const auto INDEX = std::ranges::find_if(saved->members, [&](const auto& member) { return member.first == request.target; }) - saved->members.begin();
                CURRENT->remove(request.target);
                ORIGINAL->add(DATA, sc<int>(INDEX) - 1);
                TAPE->remove(CURRENT);
                for (size_t i = 0; i < saved->members.size(); ++i)
                    ORIGINAL->setTargetSize(i, saved->members[i].second);
                ORIGINAL->lastFocusedTarget = DATA;
            }
            // Store the viewport in work-area units so pinned panels and monitor
            // size changes during fullscreen cannot leave an old pixel offset.
            controller.setOffset(saved->offset * EXTENT);
        }
    }

    static const auto PFITMETHOD  = CConfigValue<Config::INTEGER>("scrolling:focus_fit_method");
    static const auto PFSONONE    = CConfigValue<Config::INTEGER>("scrolling:fullscreen_on_one_column");
    const auto        TAPE_EXTENT = controller.calculateMaxExtent(AREA, *PFSONONE);
    // Also handle windows already fullscreen when the plugin was loaded.
    if (*PFITMETHOD == 1 && std::isfinite(TAPE_EXTENT) && TAPE_EXTENT >= EXTENT)
        controller.setOffset(std::clamp(controller.getOffset(), 0.0, TAPE_EXTENT - EXTENT));
    return RESULT;
}

static CFunctionHook* createHook(const std::string& name, const std::string& signature, void* replacement) {
    void* address = nullptr;
    for (const auto& fn : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name)) {
        const std::unique_ptr<char, decltype(&std::free)> NAME{abi::__cxa_demangle(fn.signature.c_str(), nullptr, nullptr, nullptr), std::free};
        if (!NAME || !std::string_view{NAME.get()}.starts_with(signature))
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
    g_pScrollingFullscreenHook =
        createHook("requestFullscreen", "Fullscreen::ScrollingFullscreenHandler::CScrollingFullscreenHandler::requestFullscreen(", rc<void*>(hkFullscreen));
    if (!g_pScrollingRecalculateHook || !g_pScrollingDataHook || !g_pScrollingFocusHook || !g_pScrollingFullscreenHook || !g_pScrollingRecalculateHook->hook() ||
        !g_pScrollingDataHook->hook() || !g_pScrollingFocusHook->hook() || !g_pScrollingFullscreenHook->hook()) {
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
    if (g_pScrollingFullscreenHook)
        g_pScrollingFullscreenHook->unhook();
    if (g_pScrollingFocusHook)
        g_pScrollingFocusHook->unhook();
    if (g_pScrollingDataHook)
        g_pScrollingDataHook->unhook();
    if (g_pScrollingRecalculateHook)
        g_pScrollingRecalculateHook->unhook();
    g_viewports.clear();
    g_restoreStates.clear();
}
