#include "OverviewRender.hpp"
#include <chrono>
#include <cmath>
#include <sstream>
// 0.55: more of the GL/renderer surface area went private/protected.
// Keep the well-worn unwrap-the-access-modifiers trick the plugin uses.
#define protected public
#define private public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/gl/GLFramebuffer.hpp> // 0.55: CFramebuffer renamed to Render::CGLFramebuffer
#include <hyprland/src/helpers/math/Math.hpp>
#undef private
#undef protected

// 0.55: g_pHyprOpenGL now lives in Render::GL::; SRenderModifData and the
// SRenderData fields all live in Render::. Pull both in unqualified.
using namespace Render;
using namespace Render::GL;

namespace OverviewRender {

void flushPass(PHLMONITOR monitor) {
    if (!monitor || g_pHyprRenderer->m_renderPass.empty())
        return;

    g_pHyprRenderer->m_renderPass.render(CRegion{CBox{{}, monitor->m_transformedSize}});
    g_pHyprRenderer->m_renderPass.clear();
}

void renderBlur(PHLMONITOR monitor, const CBox& windowBox, int rounding, float roundingPower, float alpha, bool usePrecomputedBlur) {
    if (!monitor || alpha <= 0.F)
        return;

    (void)usePrecomputedBlur; // 0.55: per-monitor cached blur (m_renderData.pCurrentMonData->blurFB) is gone, and getBlurTexture() is protected on IHyprRenderer; we always take the on-demand path now. Keep the parameter so callers don't have to change.

    CRegion blurDamage{windowBox};
    if (blurDamage.empty())
        return;

    CRegion drawDamage{CBox{{}, monitor->m_transformedSize}};

    // 0.55: blurMainFramebufferWithDamage went private and now needs a
    // CGLFramebuffer& source argument; the public renderer entry point
    // (blurMainFramebuffer) blurs whatever m_renderData.currentFB is
    // currently bound to. That's a subtle behaviour change from the 0.54
    // implementation which always blurred the monitor's main framebuffer.
    //
    // In this plugin's call sequence renderBlur fires while currentFB is
    // pointing at a scratch / temporary FB (see /tmp/scrolloverview.log
    // diagnostic during the 0.55 port: currentFB != mainFB, but currentFB
    // is allocated and has a texture — just not the rendered desktop).
    // Blurring that produces "noise" — blurMainFramebuffer succeeds and
    // returns a non-null texture, but it's a blur of uninitialised pixel
    // data because nothing has yet been rendered into the scratch FB.
    //
    // Pin currentFB to mainFB for the duration of the blurMainFramebuffer
    // call so it reads from the monitor's actual content. Restore after.
    // We can't store/restore SP<IFramebuffer> by raw pointer round-trip
    // without exposing the renderer's internals, so check which SP owned
    // the previous raw pointer and rebind via that.
    auto* const SAVED_CURRENTFB = g_pHyprRenderer->m_renderData.currentFB.get();
    if (g_pHyprRenderer->m_renderData.mainFB)
        g_pHyprRenderer->m_renderData.currentFB = g_pHyprRenderer->m_renderData.mainFB;
    const auto BLURREDTEXTURE = g_pHyprRenderer ? g_pHyprRenderer->blurMainFramebuffer(alpha, &blurDamage) : SP<ITexture>{};
    if (SAVED_CURRENTFB) {
        if (g_pHyprRenderer->m_renderData.mainFB && g_pHyprRenderer->m_renderData.mainFB.get() == SAVED_CURRENTFB)
            g_pHyprRenderer->m_renderData.currentFB = g_pHyprRenderer->m_renderData.mainFB;
        else if (g_pHyprRenderer->m_renderData.outFB && g_pHyprRenderer->m_renderData.outFB.get() == SAVED_CURRENTFB)
            g_pHyprRenderer->m_renderData.currentFB = g_pHyprRenderer->m_renderData.outFB;
    }
    if (!BLURREDTEXTURE)
        return;

    CBox transformedBox = windowBox;
    transformedBox.transform(Math::wlTransformToHyprutils(Math::invertTransform(monitor->m_transform)), monitor->m_transformedSize.x, monitor->m_transformedSize.y);

    const CBox monitorSpaceBox = {transformedBox.pos().x / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.pos().y / monitor->m_pixelSize.y * monitor->m_transformedSize.y,
                                  transformedBox.width / monitor->m_pixelSize.x * monitor->m_transformedSize.x,
                                  transformedBox.height / monitor->m_pixelSize.y * monitor->m_transformedSize.y};

    CHyprOpenGLImpl::STextureRenderData renderData;
    renderData.damage                               = &drawDamage;
    renderData.a                                    = alpha;
    renderData.round                                = rounding;
    renderData.roundingPower                        = roundingPower;
    renderData.allowCustomUV                        = true;
    renderData.allowDim                             = false;

    // 0.55: m_renderData (and the push/pop monitor-transform helpers) live on
    // IHyprRenderer now; the GL impl just reads them.
    g_pHyprRenderer->pushMonitorTransformEnabled(true);
    const auto SAVEDRENDERMODIF                              = g_pHyprRenderer->m_renderData.renderModif;
    const auto SAVEDUVTOPLEFT                                = g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft;
    const auto SAVEDUVBOTTOMRIGHT                            = g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight;
    g_pHyprRenderer->m_renderData.renderModif                = {};
    g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = monitorSpaceBox.pos() / monitor->m_transformedSize;
    g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = (monitorSpaceBox.pos() + monitorSpaceBox.size()) / monitor->m_transformedSize;
    g_pHyprOpenGL->renderTexture(BLURREDTEXTURE, windowBox, renderData);
    g_pHyprRenderer->m_renderData.primarySurfaceUVTopLeft     = SAVEDUVTOPLEFT;
    g_pHyprRenderer->m_renderData.primarySurfaceUVBottomRight = SAVEDUVBOTTOMRIGHT;
    g_pHyprRenderer->m_renderData.renderModif                 = SAVEDRENDERMODIF;
    g_pHyprRenderer->popMonitorTransformEnabled();
}

}
