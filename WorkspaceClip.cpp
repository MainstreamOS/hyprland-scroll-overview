#include "globals.hpp"
#include "WorkspaceClip.hpp"

#include <optional>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <string_view>
#include <hyprland/src/desktop/Workspace.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

struct SWorkspaceClip {
    CBox logical;
    CBox pixels;
};
static std::optional<SWorkspaceClip> g_workspaceClip;
static CFunctionHook*                g_windowHook = nullptr;
static CFunctionHook*                g_addHook    = nullptr;
static CFunctionHook*                g_drawHook   = nullptr;
using origWindow                                  = void (*)(Render::IHyprRenderer*, PHLWINDOW, PHLMONITOR, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
using origAdd                                     = void (*)(Render::CRenderPass*, UP<IPassElement>&&);
using origDraw                                    = void (*)(Render::IElementRenderer*, WP<IPassElement>, const CRegion&);

class CWorkspaceClipElement : public IPassElement {
  public:
    CWorkspaceClipElement(UP<IPassElement>&& element, const SWorkspaceClip& clip) : inner(std::move(element)), clip(clip) {}
    bool needsLiveBlur() override {
        return inner->needsLiveBlur();
    }
    bool needsPrecomputeBlur() override {
        return inner->needsPrecomputeBlur();
    }
    bool undiscardable() override {
        return inner->undiscardable();
    }
    bool disableSimplification() override {
        return inner->disableSimplification();
    }
    void discard() override {
        inner->discard();
    }
    std::optional<CBox> boundingBox() override {
        const auto BOX = inner->boundingBox();
        return BOX ? BOX->intersection(clip.logical) : std::optional<CBox>{clip.logical};
    }
    CRegion opaqueRegion() override {
        return inner->opaqueRegion().intersect(clip.logical);
    }
    const char* passName() override {
        return "CWorkspaceClipElement";
    }
    ePassElementType type() override {
        return EK_CUSTOM;
    }
    UP<IPassElement> inner;
    SWorkspaceClip   clip;
};

static void hkWindow(Render::IHyprRenderer* renderer, PHLWINDOW window, PHLMONITOR monitor, const Time::steady_tp& time, bool decorate, Render::eRenderPassMode mode,
                     bool ignorePosition, bool standalone) {
    const auto                    PREVIOUS = g_workspaceClip;
    Hyprutils::Utils::CScopeGuard restore([&] { g_workspaceClip = PREVIOUS; });
    g_workspaceClip.reset();
    const auto WORKSPACE = window->m_workspace;
    // Captures and transformed windows render in their own coordinate space.
    // Ordinary scrolling windows must stay inside their moving workspace's
    // viewport, so off-screen columns cannot slide across its boundary.
    if (!ignorePosition && !standalone && !window->m_isFloating && window->m_transformers.empty() && WORKSPACE && !WORKSPACE->m_isSpecialWorkspace &&
        !WORKSPACE->m_forceRendering && WORKSPACE->m_renderOffset->isBeingAnimated() && WORKSPACE->m_space && WORKSPACE->m_space->algorithm() &&
        dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(WORKSPACE->m_space->algorithm()->tiledAlgo().get())) {
        const CBox LOGICAL = {WORKSPACE->m_renderOffset->value(), monitor->m_size};
        g_workspaceClip    = SWorkspaceClip{LOGICAL, CBox{LOGICAL}.scale(monitor->m_scale).round()};
    }
    rc<origWindow>(g_windowHook->m_original)(renderer, window, monitor, time, decorate, mode, ignorePosition, standalone);
}

static void hkAdd(Render::CRenderPass* pass, UP<IPassElement>&& element) {
    if (g_workspaceClip && element->type() != EK_HINTS && element->type() != EK_FRAMEBUFFER && element->type() != EK_PRE_BLUR)
        element = makeUnique<CWorkspaceClipElement>(std::move(element), *g_workspaceClip);
    rc<origAdd>(g_addHook->m_original)(pass, std::move(element));
}

static void hkDraw(Render::IElementRenderer* renderer, WP<IPassElement> element, const CRegion& damage) {
    if (element && element->type() == EK_CUSTOM) {
        if (const auto CLIPPED = dynamic_cast<CWorkspaceClipElement*>(element.get())) {
            const auto DAMAGE = damage.copy().intersect(CLIPPED->clip.pixels);
            if (DAMAGE.empty()) {
                CLIPPED->discard();
                return;
            }
            // Surface and texture rendering also read the current pass damage.
            auto&                         data     = g_pHyprRenderer->m_renderData;
            const auto                    PREVIOUS = data.damage.copy();
            Hyprutils::Utils::CScopeGuard restore([&] { data.damage = PREVIOUS; });
            data.damage = DAMAGE;
            rc<origDraw>(g_drawHook->m_original)(renderer, CLIPPED->inner, DAMAGE);
            return;
        }
    }
    rc<origDraw>(g_drawHook->m_original)(renderer, element, damage);
}

static CFunctionHook* createHook(const std::string& name, const std::string& signature, void* replacement) {
    void* address = nullptr;
    for (const auto& fn : HyprlandAPI::findFunctionsByName(SCROLLOVERVIEW_HANDLE, name)) {
        // Hyprland's cached demangled list can be out of step with its raw
        // symbols. Match the actual symbol instead of an unrelated entry.
        const std::unique_ptr<char, decltype(&std::free)> NAME{abi::__cxa_demangle(fn.signature.c_str(), nullptr, nullptr, nullptr), std::free};
        if (!NAME || !std::string_view{NAME.get()}.starts_with(signature))
            continue;
        if (address)
            return nullptr;
        address = fn.address;
    }
    return address ? HyprlandAPI::createFunctionHook(SCROLLOVERVIEW_HANDLE, address, replacement) : nullptr;
}

bool enableWorkspaceClip() {
    g_windowHook = createHook("renderWindow", "Render::IHyprRenderer::renderWindow(", rc<void*>(hkWindow));
    g_addHook    = createHook("add", "Render::CRenderPass::add(", rc<void*>(hkAdd));
    g_drawHook   = createHook("drawElement", "Render::IElementRenderer::drawElement(", rc<void*>(hkDraw));
    if (!g_windowHook || !g_addHook || !g_drawHook || !g_windowHook->hook() || !g_addHook->hook() || !g_drawHook->hook()) {
        disableWorkspaceClip();
        return false;
    }
    return true;
}

void disableWorkspaceClip() {
    g_pHyprRenderer->m_renderPass.removeAllOfType("CWorkspaceClipElement");
    if (g_drawHook)
        g_drawHook->unhook();
    if (g_addHook)
        g_addHook->unhook();
    if (g_windowHook)
        g_windowHook->unhook();
    g_workspaceClip.reset();
}
