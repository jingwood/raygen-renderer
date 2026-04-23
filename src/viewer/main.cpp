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
struct ViewerParams {
    int   samples       = 4;
    float resScale      = 0.5f;    // multiplies the scene's base resolution
    int   baseWidth     = 1200;
    int   baseHeight    = 750;
    bool  denoise       = true;
    bool  postProcess   = false;
    float exposure      = 1.0f;
    float envIntensity  = 0.3f;
    float envRotation   = 120.0f;
    float bloomThreshold = 0.7f;
    float bloomStrength  = 0.35f;
};

// Result handed from the worker back to the main thread. A boolean ready flag
// lives in the parent RenderJob; this struct just owns the pixels.
struct RenderResult {
    std::vector<unsigned char> rgba;   // tight RGBA8
    int width  = 0;
    int height = 0;
    double secs = 0.0;
};

// Render job state machine:
//   Idle: worker is asleep. Main thread sets `pending` + notifies to start.
//   Busy: worker is rendering. Main thread shows spinner + previous image.
//   Ready: worker wrote into `result` and set ready=true. Main thread copies
//          pixels to GL texture and flips back to Idle.
struct RenderJob {
    std::mutex mu;
    std::condition_variable cv;
    ViewerParams pending;
    bool hasPending = false;
    bool running    = false;      // worker is mid-render
    bool ready      = false;      // fresh result waiting for the main thread
    bool quit       = false;
    RenderResult result;
};

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

static void applyParamsToScene(const ViewerParams& p, RendererSettings& rs, Scene& scene) {
    rs.samples        = p.samples;
    rs.resolutionWidth  = (int)(p.baseWidth  * p.resScale);
    rs.resolutionHeight = (int)(p.baseHeight * p.resScale);
    if (rs.resolutionWidth  < 16) rs.resolutionWidth  = 16;
    if (rs.resolutionHeight < 16) rs.resolutionHeight = 16;
    rs.enableDenoise              = p.denoise;
    rs.enableRenderingPostProcess = p.postProcess;
    rs.bloomThreshold             = p.bloomThreshold;
    rs.bloomStrength              = p.bloomStrength;

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

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Scene + renderer live here on the main thread; the worker only touches
    // them while it holds the job mutex. That's enough for a cooperative
    // single-render-at-a-time model - no in-flight render is mutated.
    RendererSettings rs;
    RayRenderer renderer(&rs);
    Scene scene;
    RendererSceneLoader loader;
    loader.load(renderer, &scene, scenePath);
    renderer.setScene(&scene);

    // Seed UI params from the just-loaded scene so sliders start where the
    // scene.json left off.
    ViewerParams uiParams;
    uiParams.envIntensity = scene.envmapIntensity;
    uiParams.envRotation  = scene.envmapRotation;
    if (scene.mainCamera) uiParams.exposure = scene.mainCamera->exposure;

    RenderJob job;

    std::thread worker([&]() {
        ViewerParams p;
        while (true) {
            {
                std::unique_lock<std::mutex> lk(job.mu);
                job.cv.wait(lk, [&]{ return job.quit || job.hasPending; });
                if (job.quit) return;
                p = job.pending;
                job.hasPending = false;
                job.running = true;
            }

            // Apply snapshot and render. renderer/scene/rs are touched only
            // while running==true and main thread will not modify them.
            applyParamsToScene(p, rs, scene);

            double t0 = glfwGetTime();
            renderer.render();
            double secs = glfwGetTime() - t0;

            {
                std::lock_guard<std::mutex> lk(job.mu);
                packImageToRGBA(renderer.getRenderResult(), job.result.rgba,
                                job.result.width, job.result.height);
                job.result.secs = secs;
                job.running = false;
                job.ready = true;
            }
        }
    });

    // Kick an initial render so something appears on screen immediately.
    auto enqueue = [&](const ViewerParams& p) {
        std::lock_guard<std::mutex> lk(job.mu);
        job.pending = p;
        job.hasPending = true;
        job.cv.notify_one();
    };
    enqueue(uiParams);

    GLuint renderTex = 0;
    int   texW = 0, texH = 0;
    double lastRenderSec = 0.0;
    bool  isRendering = true;
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
                lastRenderSec = job.result.secs;
                job.ready = false;
            }
            isRendering = job.running || job.hasPending;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // --- Control panel ---
        ImGui::Begin("raygen viewer");
        ImGui::Text("scene: %s", scenePath);
        ImGui::Text("FPS: %.1f   last render: %.2f s%s",
                    io.Framerate, lastRenderSec, isRendering ? "  (rendering...)" : "");
        ImGui::Separator();

        bool dirty = false;
        dirty |= ImGui::SliderInt  ("samples",       &uiParams.samples, 1, 200);
        dirty |= ImGui::SliderFloat("resolution x",  &uiParams.resScale, 0.1f, 1.5f, "%.2f");
        dirty |= ImGui::SliderFloat("exposure",      &uiParams.exposure, 0.1f, 3.0f, "%.2f");
        dirty |= ImGui::SliderFloat("envmap intensity", &uiParams.envIntensity, 0.0f, 3.0f, "%.2f");
        dirty |= ImGui::SliderFloat("envmap rotation", &uiParams.envRotation, 0.0f, 360.0f, "%.0f");
        dirty |= ImGui::Checkbox   ("denoise",       &uiParams.denoise);
        ImGui::SameLine();
        dirty |= ImGui::Checkbox   ("post-process",  &uiParams.postProcess);

        if (uiParams.postProcess) {
            dirty |= ImGui::SliderFloat("bloom threshold", &uiParams.bloomThreshold, 0.0f, 2.0f, "%.2f");
            dirty |= ImGui::SliderFloat("bloom strength",  &uiParams.bloomStrength,  0.0f, 1.0f, "%.2f");
        }

        ImGui::Separator();
        const bool canKick = !isRendering;
        if (!canKick) ImGui::BeginDisabled();
        if (ImGui::Button("Re-render now") || (dirty && !isRendering)) {
            lastKickedParams = uiParams;
            enqueue(uiParams);
        }
        if (!canKick) ImGui::EndDisabled();

        // When the UI goes dirty but a render is already in flight, we still
        // want the latest slider values reflected on the next render. Hold the
        // intent and fire once the worker frees up.
        static bool pendingDirty = false;
        if (dirty && isRendering) pendingDirty = true;
        if (!isRendering && pendingDirty) {
            pendingDirty = false;
            lastKickedParams = uiParams;
            enqueue(uiParams);
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
