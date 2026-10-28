#include "globals.hpp"
#include "WorkspaceClip.hpp"

#include <optional>
#include <cstdlib>
#include <cxxabi.h>
#include <memory>
#include <string_view>
#include <hyprland/src/workspace/HLWorkspace.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowEffectsController.hpp>
#include <hyprland/src/render/transformer/TransformerList.hpp>
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
using origWindow                                  = void (*)(Render::IHyprRenderer*, Render::CRenderContext&, PHLWINDOW, PHLMONITOR, const Render::SWindowRenderPresentation&, const Time::steady_tp&, bool, Render::eRenderPassMode, bool, bool);
using origAdd                                     = void (*)(Render::CRenderPass*, UP<IPassElement>&&);
using origDraw                                    = void (*)(Render::IElementRenderer*, Render::CRenderContext&, WP<IPassElement>, const CRegion&);

class CWorkspaceClipElement : public IPassElement {
  public:
    CWorkspaceClipElement(UP<IPassElement>&& element, const SWorkspaceClip& clip) : inner(std::move(element)), clip(clip) {}
    bool needsLiveBlur(Render::CRenderContext& ctx) override {
        return inner->needsLiveBlur(ctx);
    }
    bool needsPrecomputeBlur(Render::CRenderContext& ctx) override {
        return inner->needsPrecomputeBlur(ctx);
    }
    bool undiscardable(Render::CRenderContext& ctx) override {
        return inner->undiscardable(ctx);
    }
    bool disableSimplification(Render::CRenderContext& ctx) override {
        return inner->disableSimplification(ctx);
    }
    bool requiresFullDamage(Render::CRenderContext& ctx) override {
        return inner->requiresFullDamage(ctx);
    }
    void discard(Render::CRenderContext& ctx) override {
        inner->discard(ctx);
    }
    std::optional<CBox> boundingBox(Render::CRenderContext& ctx) override {
        const auto BOX = inner->boundingBox(ctx);
        return BOX ? BOX->intersection(clip.logical) : std::optional<CBox>{clip.logical};
    }
    CRegion opaqueRegion(Render::CRenderContext& ctx) override {
        return inner->opaqueRegion(ctx).intersect(clip.logical);
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

static void hkWindow(Render::IHyprRenderer* renderer, Render::CRenderContext& ctx, PHLWINDOW window, PHLMONITOR monitor,
                     const Render::SWindowRenderPresentation& presentation, const Time::steady_tp& time, bool decorate, Render::eRenderPassMode mode,
                     bool ignorePosition, bool standalone) {
    const auto                    PREVIOUS = g_workspaceClip;
    Hyprutils::Utils::CScopeGuard restore([&] { g_workspaceClip = PREVIOUS; });
    g_workspaceClip.reset();
    const auto WORKSPACE = window->m_workspace;
    // Captures and transformed windows render in their own coordinate space.
    // Ordinary scrolling windows must stay inside their moving workspace's
    // viewport, so off-screen columns cannot slide across its boundary.
    if (!ctx.readOnlyEffects() && !presentation.workspaceScene && !ignorePosition && !standalone && !window->isFloating() && window->effects().transformers()->empty() && WORKSPACE && WORKSPACE->type() != Workspace::eWorkspaceType::SPECIAL &&
        !WORKSPACE->m_forceRendering && presentation.workspaceOffsetAnimating && WORKSPACE->space() && WORKSPACE->space()->algorithm() &&
        dynamic_cast<Layout::Tiled::CScrollingAlgorithm*>(WORKSPACE->space()->algorithm()->tiledAlgo().get())) {
        const CBox LOGICAL = {presentation.workspaceOffset, monitor->m_size};
        g_workspaceClip    = SWorkspaceClip{LOGICAL, CBox{LOGICAL}.scale(monitor->m_scale).round()};
    }
    rc<origWindow>(g_windowHook->m_original)(renderer, ctx, window, monitor, presentation, time, decorate, mode, ignorePosition, standalone);
}

static void hkAdd(Render::CRenderPass* pass, UP<IPassElement>&& element) {
    if (g_workspaceClip && element->type() != EK_HINTS && element->type() != EK_FRAMEBUFFER && element->type() != EK_PRE_BLUR)
        element = makeUnique<CWorkspaceClipElement>(std::move(element), *g_workspaceClip);
    rc<origAdd>(g_addHook->m_original)(pass, std::move(element));
}

static void hkDraw(Render::IElementRenderer* renderer, Render::CRenderContext& ctx, WP<IPassElement> element, const CRegion& damage) {
    if (element && element->type() == EK_CUSTOM) {
        if (const auto CLIPPED = dynamic_cast<CWorkspaceClipElement*>(element.get())) {
            const auto DAMAGE = damage.copy().intersect(CLIPPED->clip.pixels);
            if (DAMAGE.empty()) {
                CLIPPED->discard(ctx);
                return;
            }
            // Surface and texture rendering also read the current pass damage.
            auto&                         data     = ctx.m_data;
            const auto                    PREVIOUS = data.damage.copy();
            Hyprutils::Utils::CScopeGuard restore([&] { data.damage = PREVIOUS; });
            data.damage = DAMAGE;
            rc<origDraw>(g_drawHook->m_original)(renderer, ctx, CLIPPED->inner, DAMAGE);
            return;
        }
    }
    rc<origDraw>(g_drawHook->m_original)(renderer, ctx, element, damage);
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
    g_pHyprRenderer->context().m_pass.removeAllOfType("CWorkspaceClipElement");
    if (g_drawHook)
        g_drawHook->unhook();
    if (g_addHook)
        g_addHook->unhook();
    if (g_windowHook)
        g_windowHook->unhook();
    g_workspaceClip.reset();
}
