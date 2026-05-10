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
    // does the same job, manages its own framebuffer state, and returns
    // the blurred texture directly — no save/restore of currentFB needed.
    const auto BLURREDTEXTURE = g_pHyprRenderer ? g_pHyprRenderer->blurMainFramebuffer(alpha, &blurDamage) : SP<ITexture>{};
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
