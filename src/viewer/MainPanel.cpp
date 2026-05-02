///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Main control panel implementation.
///////////////////////////////////////////////////////////////////////////////

#include "MainPanel.h"

#include <cstring>

#include "imgui.h"

#include "raygen/rayrenderer.h"
#include "raygen/scene.h"
#include "ucm/string.h"

#include "Dialog.h"

namespace raygen {
namespace viewer {

namespace {

// Status header: scene name, FPS, last render time, current state + a cancel
// button while tracing. Returns true if the cancel button was pressed (caller
// already has the renderer ref but we keep the pure-draw split clean).
void drawStatusHeader(const MainPanelCtx& ctx) {
    ImGui::Text("scene: %s", ctx.scenePath);
    ImGui::Text("FPS: %.1f   last render: %.2f s", ctx.fps, ctx.lastRenderSec);

    if (!ctx.isRendering) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "status: ready");
        ImGui::ProgressBar(1.0f, ImVec2(-1, 4));
        return;
    }

    if (ctx.currentJobKind == JobKind::PostOnly) {
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "status: post-processing...");
        ImGui::ProgressBar(1.0f, ImVec2(-1, 4), "bloom");
        return;
    }

    ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                       "status: tracing  %3.0f%%", ctx.previewProgress * 100.0f);
    ImGui::SameLine();
    // Cooperative cancel: the renderer checks cancelRequested at every row
    // boundary. Clicking x here returns within a few ms.
    if (ImGui::SmallButton("x##cancel")) {
        ctx.renderer->cancelRequested = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("cancel this render");
    ImGui::ProgressBar(ctx.previewProgress, ImVec2(-1, 4));
}

bool drawQualitySection(ViewerParams& p) {
    if (!ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    bool dirty = false;
    dirty |= ImGui::SliderInt("samples", &p.samples, 1, 1000);
    dirty |= ImGui::SliderInt("threads", &p.threads, 1, 32);
    dirty |= ImGui::Checkbox ("denoise", &p.denoise);
    if (p.denoise) {
        dirty |= ImGui::SliderFloat("denoise intensity", &p.denoiseIntensity, 0.0f, 1.0f, "%.2f");
    }
    // Adaptive sampler. `samples` becomes a cap rather than a fixed count;
    // converged tiles stop early. `base` is the per-pass step (smaller =
    // finer noise resolution but more pass overhead); `threshold` is the
    // relative SEM at which a tile is considered converged.
    dirty |= ImGui::Checkbox ("adaptive sampling", &p.adaptiveSampling);
    if (p.adaptiveSampling) {
        dirty |= ImGui::SliderInt  ("adaptive base",      &p.adaptiveBaseSamples, 1,    32);
        dirty |= ImGui::SliderFloat("adaptive threshold", &p.adaptiveThreshold,   0.001f, 0.1f, "%.3f");
    }
    return dirty;
}

bool drawCameraSection(ViewerParams& p, Camera* mainCamera) {
    if (!ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    bool dirty = false;
    // Drag widgets for position/rotation so a big scene (e.g. far-away mesh)
    // and a small scene (tabletop) both feel natural; units are world-space
    // metres for location, degrees for angle.
    dirty |= ImGui::DragFloat3("location",         p.camLocation,  0.05f, -1000.0f, 1000.0f, "%.3f");
    dirty |= ImGui::DragFloat3("angle",            p.camAngle,     0.5f,  -360.0f, 360.0f,   "%.2f");
    dirty |= ImGui::SliderFloat("fieldOfView",    &p.fieldOfView,  10.0f,  120.0f,           "%.1f");
    dirty |= ImGui::SliderFloat("depthOfField",   &p.depthOfField,  0.0f,  50.0f,            "%.2f");
    // Show a hint when the scene pinned focus to an object: the renderer
    // overwrites depthOfField each frame from that object's bbox, so the
    // slider's value won't stick until it's cleared.
    if (mainCamera && !mainCamera->focusOnObjectName.isEmpty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(focus lock: %s)", mainCamera->focusOnObjectName.getBuffer());
        ImGui::SameLine();
        if (ImGui::SmallButton("clear##focus")) {
            mainCamera->focusOnObjectName.clear();
            dirty = true;
        }
    }
    // aperture=0 disables DOF entirely (the renderer gates on
    // ctx.aperture > 0), so keeping 0 reachable lets the user get
    // pinhole-sharp output without touching depthOfField.
    dirty |= ImGui::SliderFloat("aperture",        &p.aperture,         0.0f,  22.0f, "f/%.2f");
    dirty |= ImGui::SliderInt  ("apertureBlades",  &p.apertureBlades,   0,     12);
    dirty |= ImGui::SliderFloat("apertureRotation",&p.apertureRotation, 0.0f, 360.0f, "%.1f");
    dirty |= ImGui::SliderFloat("exposure",        &p.exposure,         0.1f,  3.0f,  "%.2f");
    return dirty;
}

// Compute the directory portion of `path` into `outDir`. Used to seed the
// envmap-file dialog at the current envmap's folder (or the scene folder
// when no envmap is loaded yet).
void deriveDirOf(const char* path, char* outDir, size_t outDirCap) {
    outDir[0] = '\0';
    if (path == nullptr || outDirCap == 0) return;
    const char* lsU = std::strrchr(path, '/');
    const char* lsW = std::strrchr(path, '\\');
    const char* ls  = lsU > lsW ? lsU : lsW;
    if (ls == nullptr) return;
    size_t n = (size_t)(ls - path);
    if (n >= outDirCap) n = outDirCap - 1;
    std::memcpy(outDir, path, n);
    outDir[n] = '\0';
}

bool drawSceneSection(const MainPanelCtx& ctx) {
    if (!ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    ViewerParams& p = *ctx.params;
    bool dirty = false;

    // Envmap file row: current path (read-only display) + Browse button. The
    // Browse button is disabled while a render is in flight because the worker
    // reads scene->envmap / envmap CDF arrays per ray and the swap happens on
    // the main thread.
    const bool hasEnv = ctx.envmapPath != nullptr && ctx.envmapPath[0] != '\0';
    if (hasEnv) {
        ImGui::TextWrapped("envmap: %s", ctx.envmapPath);
    } else {
        ImGui::TextDisabled("envmap: (none)");
    }
    const bool canPickEnv = !ctx.isRendering && (bool)ctx.onLoadEnvmap;
    if (!canPickEnv) ImGui::BeginDisabled();
    if (ImGui::Button("Browse...##envmap")) {
        char initDir[512];
        // Prefer the current envmap's folder; fall back to the scene folder.
        deriveDirOf(hasEnv ? ctx.envmapPath : ctx.scenePath,
                    initDir, sizeof(initDir));
        char picked[1024] = {0};
        if (openImageFileDialog(picked, sizeof(picked),
                                "Open envmap",
                                initDir[0] ? initDir : nullptr)) {
            if (ctx.onLoadEnvmap) ctx.onLoadEnvmap(picked);
        }
    }
    if (!canPickEnv) ImGui::EndDisabled();

    dirty |= ImGui::SliderFloat("envmap intensity", &p.envIntensity, 0.0f, 10.0f,   "%.2f");
    dirty |= ImGui::SliderFloat("envmap rotation",  &p.envRotation,  0.0f, 360.0f, "%.0f");

    // Global medium (fog). σa/σs/σe are per-channel inverse world-units;
    // density is a uniform multiplier that prepare() folds into σt_hero
    // for free-flight sampling. g is the Henyey-Greenstein anisotropy
    // (0 = isotropic, >0 = forward-peaked clouds/fog, <0 = back-scatter).
    ImGui::Spacing();
    ImGui::TextDisabled("Global medium (fog)");
    dirty |= ImGui::Checkbox("enable##medium", &p.mediumEnabled);
    if (p.mediumEnabled) {
        dirty |= ImGui::DragFloat3 ("sigma_a",  p.mediumSigmaA,   0.005f, 0.0f, 10.0f, "%.4f");
        dirty |= ImGui::DragFloat3 ("sigma_s",  p.mediumSigmaS,   0.005f, 0.0f, 10.0f, "%.4f");
        dirty |= ImGui::DragFloat3 ("emission", p.mediumEmission, 0.05f,  0.0f, 100.0f, "%.3f");
        dirty |= ImGui::SliderFloat("g (HG)",  &p.mediumG,       -0.95f,  0.95f, "%.2f");
        dirty |= ImGui::SliderFloat("density", &p.mediumDensity,  0.0f,   8.0f,  "%.2f");
    }
    return dirty;
}

bool drawPostProcessSection(ViewerParams& p) {
    if (!ImGui::CollapsingHeader("Post-process (bloom)", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    bool dirty = false;
    dirty |= ImGui::Checkbox("enable##pp", &p.postProcess);
    if (p.postProcess) {
        dirty |= ImGui::SliderFloat("bloom threshold", &p.bloomThreshold, 0.0f,  10.0f,  "%.2f");
        dirty |= ImGui::SliderFloat("bloom strength",  &p.bloomStrength,  0.0f,  5.0f,  "%.2f");
        dirty |= ImGui::SliderFloat("bloom curve",     &p.bloomCurve,     1.0f,  4.0f,  "%.2f");
        dirty |= ImGui::SliderFloat("bloom radius",    &p.bloomRadius,    0.0f,  0.15f, "%.3f");
    }
    ImGui::TextDisabled("TODO: post-process-only re-run once the core\n"
                        "caches a pre-PP image");
    return dirty;
}

// Re-render button + "next kind" preview. The button always kicks Full so the
// user has an explicit "redo from scratch" lever; the preview mirrors what the
// auto-kick decision would do for the queued slider changes.
void drawKickRow(const MainPanelCtx& ctx,
                 const std::function<void(JobKind)>& kickFinal) {
    ImGui::Separator();
    const bool canKick = !ctx.isRendering;

    if (!canKick) ImGui::BeginDisabled();
    if (ImGui::Button("Re-render (full)")) {
        *ctx.lastKickedParams = *ctx.params;
        kickFinal(JobKind::Full);
        *ctx.pendingDirty = false;
    }
    if (!canKick) ImGui::EndDisabled();

    // Preview what the next auto-kick will do, so the user can tell at a
    // glance which kind is queued.
    if (*ctx.pendingDirty) {
        const JobKind nextKind =
            onlyPostProcessChanged(*ctx.lastKickedParams, *ctx.params)
                ? JobKind::PostOnly : JobKind::Full;
        ImGui::SameLine();
        ImGui::TextDisabled("(next: %s)",
                            nextKind == JobKind::PostOnly ? "post-only" : "full");
    }
}

}  // namespace

bool drawMainPanel(const MainPanelCtx& ctx,
                   const std::function<void(JobKind)>& kickFinal) {
    ImGui::Begin("raygen viewer");

    drawStatusHeader(ctx);

    bool dirty = false;
    dirty |= drawQualitySection(*ctx.params);
    dirty |= drawCameraSection(*ctx.params, ctx.mainCamera);
    dirty |= drawSceneSection(ctx);
    dirty |= drawPostProcessSection(*ctx.params);

    // Intent tracking: pendingDirty is set whenever sliders move while we
    // can't kick, and the single kick site (in main.cpp's auto-kick block)
    // fires at most once per frame.
    if (dirty) *ctx.pendingDirty = true;

    drawKickRow(ctx, kickFinal);

    // The auto-kick decision runs at end-of-frame so it catches widget
    // activity from every panel (Outline, Property, etc.) — not just this
    // one drawn first. See main.cpp, just before ImGui::Render().

    ImGui::End();
    return dirty;
}

}  // namespace viewer
}  // namespace raygen
