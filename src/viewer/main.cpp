///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer
//  Cross-platform scene viewer + tuner for the raygen renderer.
//
//  Step 4: sliders over renderer settings / camera / envmap, with the actual
//  render running on a worker thread so the UI stays responsive. The worker
//  owns the RayRenderer; the main thread queues a fresh render whenever the
//  parameters go dirty, then pulls the completed image back onto the GL
//  texture atomically.
//
//  MIT License
///////////////////////////////////////////////////////////////////////////////

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

// Windows ships with only GL 1.1 constants in <GL/gl.h>. Declare the handful of
// 1.2+ enums we use so we don't have to pull in GLEW/GLAD just for these.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#include "raygen/rayrenderer.h"
#include "raygen/sceneloader.h"
#include "ugm/image.h"

using namespace raygen;
using namespace ugm;

// -- Tunable parameters driven by the UI. All plain old data so the worker
// thread can snapshot them under a mutex without any lifetime concerns.
// Output resolution will move to a dedicated Output panel later; for now it's
// hard-coded so the preview stays snappy.
struct ViewerParams {
    int   samples          = 4;
    // Quality / denoise
    bool  denoise          = true;
    float denoiseIntensity = 1.0f;
    // Scene
    float exposure         = 1.0f;
    float envIntensity     = 0.3f;
    float envRotation      = 120.0f;
    // Post-process (bloom)
    bool  postProcess      = false;
    float bloomThreshold   = 0.7f;
    float bloomStrength    = 0.35f;
    float bloomCurve       = 1.0f;
};

// Result handed from the worker back to the main thread. A boolean ready flag
// lives in the parent RenderJob; this struct just owns the pixels.
struct RenderResult {
    std::vector<unsigned char> rgba;   // tight RGBA8
    int width  = 0;
    int height = 0;
    double secs = 0.0;
    bool  isPreview = false;           // true for mid-render snapshots
    float previewProgress = 0.0f;      // 0..1, final upload writes 1.0
};

// Full = trace + denoise + bloom. PostOnly = bloom over the cached pre-bloom
// image (skips ray tracing entirely, takes a few ms).
enum class JobKind { Full, PostOnly };

// Render job state machine:
//   Idle: worker is asleep. Main thread sets `pending` + notifies to start.
//   Busy: worker is rendering. Main thread shows spinner + previous image.
//   Ready: worker wrote into `result` and set ready=true. Main thread copies
//          pixels to GL texture and flips back to Idle.
struct RenderJob {
    std::mutex mu;
    std::condition_variable cv;
    ViewerParams pending;
    JobKind pendingKind = JobKind::Full;
    JobKind currentKind = JobKind::Full;  // what the worker is actually running
    bool hasPending = false;
    bool running    = false;      // worker is mid-render
    bool ready      = false;      // fresh result waiting for the main thread
    bool quit       = false;
    RenderResult result;
};

// Return true if the only thing that changed between `a` and `b` is a
// post-process parameter; the bloom pass can be re-run without retracing.
static bool onlyPostProcessChanged(const ViewerParams& a, const ViewerParams& b) {
    const bool pp_same =
        a.postProcess     == b.postProcess &&
        a.bloomThreshold  == b.bloomThreshold &&
        a.bloomStrength   == b.bloomStrength &&
        a.bloomCurve      == b.bloomCurve;
    const bool rest_same =
        a.samples          == b.samples &&
        a.denoise          == b.denoise &&
        a.denoiseIntensity == b.denoiseIntensity &&
        a.exposure         == b.exposure &&
        a.envIntensity     == b.envIntensity &&
        a.envRotation      == b.envRotation;
    return rest_same && !pp_same;
}

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

static unsigned char clamp_byte(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (unsigned char)(v * 255.0f);
}

static void packImageToRGBA(const Image& img, std::vector<unsigned char>& rgba,
                            int& w, int& h) {
    w = (int)img.width();
    h = (int)img.height();
    rgba.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            color4f c = img.getPixel(x, y);
            const size_t i = (size_t)(y * w + x) * 4;
            rgba[i + 0] = clamp_byte(c.r);
            rgba[i + 1] = clamp_byte(c.g);
            rgba[i + 2] = clamp_byte(c.b);
        }
    }
}

static void uploadRGBAToTexture(const unsigned char* data, int w, int h, GLuint& tex) {
    if (w <= 0 || h <= 0) return;
    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
}

// RayRenderer copies its RendererSettings by value at construction time, so
// poking the outside `rs` would be a no-op. Write into renderer.settings
// directly - that's the field the render loop actually reads.
static void applyParamsToScene(const ViewerParams& p, RayRenderer& renderer, Scene& scene) {
    RendererSettings& s = renderer.settings;
    s.samples                   = p.samples;
    s.enableDenoise             = p.denoise;
    s.denoiseIntensity          = p.denoiseIntensity;
    s.enableRenderingPostProcess = p.postProcess;
    s.bloomThreshold            = p.bloomThreshold;
    s.bloomStrength             = p.bloomStrength;
    s.bloomCurve                = p.bloomCurve;

    scene.envmapIntensity = p.envIntensity;
    scene.envmapRotation  = p.envRotation;
    if (scene.mainCamera) scene.mainCamera->exposure = p.exposure;
}

int main(int argc, char** argv) {
    const char* scenePath = (argc > 1)
        ? argv[1]
        : "F:\\3D Models\\F2\\raygen_export\\F2.json";

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1500, 950, "raygen viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    // Pick up Windows/macOS display scaling (150%, 200%, etc.) so 4K monitors
    // don't render the UI at postage-stamp size. Env override lets power users
    // dial it in manually: e.g. `set RAYGEN_UI_SCALE=2.0`.
    float uiScale = 1.0f;
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(window, &xs, &ys);
        uiScale = xs;
        if (const char* env = getenv("RAYGEN_UI_SCALE")) {
            float v = (float)atof(env);
            if (v >= 0.5f && v <= 4.0f) uiScale = v;
        }
        if (uiScale < 1.0f) uiScale = 1.0f;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Crisp text: rasterize the default font at the scaled size rather than
    // relying on FontGlobalScale (which bilinear-stretches a small atlas).
    {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * uiScale;
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui::GetStyle().ScaleAllSizes(uiScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Scene + renderer live here on the main thread; the worker only touches
    // them while it holds the job mutex. That's enough for a cooperative
    // single-render-at-a-time model - no in-flight render is mutated.
    RendererSettings rsInit;
    rsInit.resolutionWidth  = 600;
    rsInit.resolutionHeight = 375;
    RayRenderer renderer(&rsInit);
    Scene scene;
    RendererSceneLoader loader;
    loader.load(renderer, &scene, scenePath);
    renderer.setScene(&scene);

    // Seed UI params from the just-loaded scene/renderer so sliders start
    // where the scene.json (and RendererSettings defaults) left off.
    ViewerParams uiParams;
    uiParams.samples          = renderer.settings.samples;
    uiParams.denoise          = renderer.settings.enableDenoise;
    uiParams.denoiseIntensity = renderer.settings.denoiseIntensity;
    uiParams.postProcess      = renderer.settings.enableRenderingPostProcess;
    uiParams.bloomThreshold   = renderer.settings.bloomThreshold;
    uiParams.bloomStrength    = renderer.settings.bloomStrength;
    uiParams.bloomCurve       = renderer.settings.bloomCurve;
    uiParams.envIntensity     = scene.envmapIntensity;
    uiParams.envRotation      = scene.envmapRotation;
    if (scene.mainCamera) uiParams.exposure = scene.mainCamera->exposure;

    RenderJob job;

    // Progressive preview: while the render thread pool is still filling
    // pixels, periodically snapshot `renderingImage` into the job result so
    // the UI can show intermediate frames. Pixel reads race with writers and
    // may tear, but for a preview that is acceptable - the final upload is
    // clean because the render has fully returned by then.
    std::thread worker([&]() {
        ViewerParams p;
        JobKind kind = JobKind::Full;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(job.mu);
                job.cv.wait(lk, [&]{ return job.quit || job.hasPending; });
                if (job.quit) return;
                p = job.pending;
                kind = job.pendingKind;
                job.hasPending = false;
                job.running = true;
                job.currentKind = kind;
                // Fresh progress for Full; PostOnly doesn't need a bar at all.
                job.result.previewProgress = (kind == JobKind::PostOnly) ? 1.0f : 0.0f;
            }

            // Apply snapshot and render. renderer/scene are touched only
            // while running==true and main thread will not modify them.
            applyParamsToScene(p, renderer, scene);

            double t0 = glfwGetTime();
            if (kind == JobKind::PostOnly) {
                // reapplyPostProcess returns false the first time (no cached
                // pre-bloom image yet); fall back to a full render so the
                // UI still produces something.
                if (!renderer.reapplyPostProcess()) {
                    renderer.render();
                }
            } else {
                // Progressive preview for full renders only. Bloom-only is
                // too fast for mid-frame snapshotting to matter.
                double lastPreview = glfwGetTime();
                renderer.progressCallback = [&](float progress) {
                    const double now = glfwGetTime();
                    if (now - lastPreview < 0.2) return;
                    lastPreview = now;

                    std::vector<unsigned char> rgba;
                    int w = 0, h = 0;
                    packImageToRGBA(renderer.getRenderResult(), rgba, w, h);

                    std::lock_guard<std::mutex> lk(job.mu);
                    job.result.rgba = std::move(rgba);
                    job.result.width = w;
                    job.result.height = h;
                    job.result.secs = now;
                    job.result.isPreview = true;
                    job.result.previewProgress = progress;
                    job.ready = true;
                };
                renderer.render();
                renderer.progressCallback = nullptr;
            }
            double secs = glfwGetTime() - t0;

            {
                std::lock_guard<std::mutex> lk(job.mu);
                packImageToRGBA(renderer.getRenderResult(), job.result.rgba,
                                job.result.width, job.result.height);
                job.result.secs = secs;
                job.result.isPreview = false;
                job.result.previewProgress = 1.0f;
                job.running = false;
                job.ready = true;
            }
        }
    });

    // Kick an initial render so something appears on screen immediately.
    auto enqueue = [&](const ViewerParams& p, JobKind kind = JobKind::Full) {
        std::lock_guard<std::mutex> lk(job.mu);
        job.pending = p;
        job.pendingKind = kind;
        job.hasPending = true;
        job.cv.notify_one();
    };
    enqueue(uiParams);

    GLuint renderTex = 0;
    int   texW = 0, texH = 0;
    double lastRenderSec = 0.0;
    bool  isRendering = true;
    JobKind currentJobKind = JobKind::Full;
    float previewProgress = 0.0f;
    bool  lastUploadWasPreview = false;
    ViewerParams lastKickedParams = uiParams;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Pull any ready result onto the GL texture.
        {
            std::unique_lock<std::mutex> lk(job.mu);
            if (job.ready) {
                uploadRGBAToTexture(job.result.rgba.data(),
                                    job.result.width, job.result.height, renderTex);
                texW = job.result.width;
                texH = job.result.height;
                if (!job.result.isPreview) {
                    lastRenderSec = job.result.secs;
                }
                previewProgress = job.result.previewProgress;
                lastUploadWasPreview = job.result.isPreview;
                job.ready = false;
            }
            isRendering = job.running || job.hasPending;
            currentJobKind = job.currentKind;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Control panel ---
        ImGui::Begin("raygen viewer");
        ImGui::Text("scene: %s", scenePath);
        ImGui::Text("FPS: %.1f   last render: %.2f s", io.Framerate, lastRenderSec);
        if (isRendering) {
            if (currentJobKind == JobKind::PostOnly) {
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "status: post-processing...");
                ImGui::ProgressBar(1.0f, ImVec2(-1, 4), "bloom");
            } else {
                ImGui::TextColored(ImVec4(0.9f, 0.8f, 0.3f, 1.0f),
                                   "status: tracing  %3.0f%%", previewProgress * 100.0f);
                ImGui::ProgressBar(previewProgress, ImVec2(-1, 4));
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "status: ready");
            ImGui::ProgressBar(1.0f, ImVec2(-1, 4));
        }

        bool dirty = false;

        if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::SliderInt("samples", &uiParams.samples, 1, 1000);
            dirty |= ImGui::Checkbox ("denoise", &uiParams.denoise);
            if (uiParams.denoise) {
                dirty |= ImGui::SliderFloat("denoise intensity", &uiParams.denoiseIntensity, 0.0f, 1.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::SliderFloat("exposure",         &uiParams.exposure,     0.1f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("envmap intensity", &uiParams.envIntensity, 0.0f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("envmap rotation",  &uiParams.envRotation,  0.0f, 360.0f, "%.0f");
        }

        if (ImGui::CollapsingHeader("Post-process (bloom)", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::Checkbox("enable##pp", &uiParams.postProcess);
            if (uiParams.postProcess) {
                dirty |= ImGui::SliderFloat("bloom threshold", &uiParams.bloomThreshold, 0.0f, 2.0f, "%.2f");
                dirty |= ImGui::SliderFloat("bloom strength",  &uiParams.bloomStrength,  0.0f, 1.0f, "%.2f");
                dirty |= ImGui::SliderFloat("bloom curve",     &uiParams.bloomCurve,     0.1f, 4.0f, "%.2f");
            }
            ImGui::TextDisabled("TODO: post-process-only re-run once the core\n"
                                "caches a pre-PP image");
        }

        ImGui::Separator();
        const bool canKick = !isRendering;

        // Intent tracking: `pendingDirty` is set whenever sliders move while
        // we can't kick, and the single kick site below fires at most once
        // per frame. The previous code fired both an immediate kick *and*
        // the pending kick in the same frame after a PostOnly finished,
        // which left the second kick comparing lastKickedParams (just
        // updated) against itself, reporting "no bloom diff", and falling
        // back to JobKind::Full - a full re-trace with fresh noise.
        static bool pendingDirty = false;
        if (dirty) pendingDirty = true;

        if (!canKick) ImGui::BeginDisabled();
        if (ImGui::Button("Re-render (full)")) {
            lastKickedParams = uiParams;
            enqueue(uiParams, JobKind::Full);
            pendingDirty = false;
        }
        if (!canKick) ImGui::EndDisabled();

        // Preview what the next auto-kick will do, so the user can tell at
        // a glance which kind is queued.
        if (pendingDirty) {
            const JobKind nextKind =
                onlyPostProcessChanged(lastKickedParams, uiParams)
                    ? JobKind::PostOnly : JobKind::Full;
            ImGui::SameLine();
            ImGui::TextDisabled("(next: %s)",
                                nextKind == JobKind::PostOnly ? "post-only" : "full");
        }

        if (!isRendering && pendingDirty) {
            const JobKind kind =
                onlyPostProcessChanged(lastKickedParams, uiParams)
                    ? JobKind::PostOnly : JobKind::Full;
            lastKickedParams = uiParams;
            enqueue(uiParams, kind);
            pendingDirty = false;
        }

        ImGui::End();

        // --- Render preview ---
        ImGui::Begin("render", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
        if (renderTex != 0) {
            ImGui::Image((ImTextureID)(intptr_t)renderTex, ImVec2((float)texW, (float)texH));
        } else {
            ImGui::Text("(warming up...)");
        }
        ImGui::End();

        ImGui::Render();
        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.10f, 0.11f, 0.13f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    {
        std::lock_guard<std::mutex> lk(job.mu);
        job.quit = true;
        job.cv.notify_all();
    }
    worker.join();

    if (renderTex) glDeleteTextures(1, &renderTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
