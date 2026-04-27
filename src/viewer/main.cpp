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
#include <functional>
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

#include <memory>

#include "raygen/medium.h"
#include "raygen/rayrenderer.h"
#include "raygen/sceneloader.h"
#include "ugm/image.h"
#include "ugm/imgcodec.h"
#include "ucm/file.h"
#include "ucm/jsonreader.h"
#include "ucm/jsonwriter.h"
#include "ucm/jstypes.h"
#include "ucm/string.h"

using namespace raygen;
using namespace ugm;

// -- Tunable parameters driven by the UI. All plain old data so the worker
// thread can snapshot them under a mutex without any lifetime concerns.
// Output resolution will move to a dedicated Output panel later; for now it's
// hard-coded so the preview stays snappy.
struct ViewerParams {
    int   samples          = 4;
    int   threads          = 7;
    // Quality / denoise
    bool  denoise          = true;
    float denoiseIntensity = 1.0f;
    // Camera (mainCamera). Angles in degrees; aperture is an f-stop-like
    // value (smaller = wider blur); apertureBlades=0 is a round iris.
    float camLocation[3]   = {0.0f, 0.0f, 0.0f};
    float camAngle[3]      = {0.0f, 0.0f, 0.0f};
    float fieldOfView      = 45.0f;
    float depthOfField     = 0.0f;
    float aperture         = 1.8f;
    int   apertureBlades   = 0;
    float apertureRotation = 0.0f;
    float exposure         = 1.0f;
    // Scene
    float envIntensity     = 0.3f;
    float envRotation      = 120.0f;
    // Global participating medium (fog). When `mediumEnabled` is false the
    // viewer leaves scene.globalMedium NULL — same code path as a non-volumetric
    // scene. The σa/σs/σe sliders edit color3 channels directly; HG anisotropy
    // and density mirror the JSON loader's fields.
    bool  mediumEnabled    = false;
    float mediumSigmaA[3]  = {0.0f, 0.0f, 0.0f};
    float mediumSigmaS[3]  = {0.0f, 0.0f, 0.0f};
    float mediumEmission[3] = {0.0f, 0.0f, 0.0f};
    float mediumG          = 0.0f;
    float mediumDensity    = 1.0f;
    // Post-process (bloom) — HDR energy-based, not LDR. Threshold is the
    // linear-radiance luma above which the excess energy becomes halo; a
    // diffuse white surface sits around 1, so threshold=1 means "only bloom
    // things brighter than white" (emitters, sun hits, specular highlights).
    bool  postProcess      = false;
    float bloomThreshold   = 1.0f;
    float bloomStrength    = 1.0f;
    float bloomCurve       = 1.0f;
    float bloomRadius      = 0.03f;
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
        a.bloomCurve      == b.bloomCurve &&
        a.bloomRadius     == b.bloomRadius;
    const bool cam_same =
        a.camLocation[0]   == b.camLocation[0] &&
        a.camLocation[1]   == b.camLocation[1] &&
        a.camLocation[2]   == b.camLocation[2] &&
        a.camAngle[0]      == b.camAngle[0] &&
        a.camAngle[1]      == b.camAngle[1] &&
        a.camAngle[2]      == b.camAngle[2] &&
        a.fieldOfView      == b.fieldOfView &&
        a.depthOfField     == b.depthOfField &&
        a.aperture         == b.aperture &&
        a.apertureBlades   == b.apertureBlades &&
        a.apertureRotation == b.apertureRotation;
    const bool medium_same =
        a.mediumEnabled     == b.mediumEnabled &&
        a.mediumSigmaA[0]   == b.mediumSigmaA[0] &&
        a.mediumSigmaA[1]   == b.mediumSigmaA[1] &&
        a.mediumSigmaA[2]   == b.mediumSigmaA[2] &&
        a.mediumSigmaS[0]   == b.mediumSigmaS[0] &&
        a.mediumSigmaS[1]   == b.mediumSigmaS[1] &&
        a.mediumSigmaS[2]   == b.mediumSigmaS[2] &&
        a.mediumEmission[0] == b.mediumEmission[0] &&
        a.mediumEmission[1] == b.mediumEmission[1] &&
        a.mediumEmission[2] == b.mediumEmission[2] &&
        a.mediumG           == b.mediumG &&
        a.mediumDensity     == b.mediumDensity;
    const bool rest_same =
        a.samples          == b.samples &&
        a.threads          == b.threads &&
        a.denoise          == b.denoise &&
        a.denoiseIntensity == b.denoiseIntensity &&
        a.exposure         == b.exposure &&
        a.envIntensity     == b.envIntensity &&
        a.envRotation      == b.envRotation &&
        cam_same &&
        medium_same;
    return rest_same && !pp_same;
}

// Sidecar companion file: "<scene>.viewer.json" next to the scene JSON.
// Strips the last extension inside the basename and appends ".viewer.json";
// if the path has no extension we just append. This keeps scene.json free of
// viewer state (and free of JSONC comments vs. JSON round-trip trouble).
static void computeSidecarPath(const char* scenePath, char* out, size_t outCap) {
    const char* lastDot = strrchr(scenePath, '.');
    const char* lastSlashU = strrchr(scenePath, '/');
    const char* lastSlashW = strrchr(scenePath, '\\');
    const char* lastSlash = lastSlashU > lastSlashW ? lastSlashU : lastSlashW;
    const bool dotInBasename = lastDot && (!lastSlash || lastDot > lastSlash);
    const size_t baseLen = dotInBasename ? (size_t)(lastDot - scenePath)
                                         : strlen(scenePath);
    const char* suffix = ".viewer.json";
    const size_t suffixLen = strlen(suffix);
    size_t keep = baseLen;
    if (keep + suffixLen + 1 > outCap) keep = outCap - suffixLen - 1;
    memcpy(out, scenePath, keep);
    memcpy(out + keep, suffix, suffixLen + 1);  // includes trailing NUL
}

// Read a JSON array of 3 numbers into `v`. Silently leaves `v` untouched if
// the key is missing or the array is the wrong shape — defaults stay in place.
static void readVec3Into(const ucm::JSObject* obj, const char* key, float v[3]) {
    std::vector<ucm::JSValue>* arr = obj->getArrayProperty(key);
    if (!arr || arr->size() < 3) return;
    for (int i = 0; i < 3; i++) {
        const ucm::JSValue& val = (*arr)[i];
        if (val.type == ucm::JSType::JSType_Number) v[i] = (float)val.number;
    }
}

static void writeVec3(ucm::JSONWriter& w, const char* key, const float v[3]) {
    w.beginArrayWithKey(ucm::string(key));
    w.writeArrayElement((double)v[0]);
    w.writeArrayElement((double)v[1]);
    w.writeArrayElement((double)v[2]);
    w.endArray();
}

// Pull the sidecar's known fields into `params` (only overwrites keys that
// are present, so older sidecars stay forward-compatible). Returns true if
// the file existed and parsed — on failure, defaults stay in place.
static bool loadViewerConfig(const char* path, ViewerParams& params,
                             int& outputWidth, int& outputHeight) {
    ucm::string pathStr(path);
    ucm::File f(pathStr);
    if (!f.isExist()) return false;
    ucm::string json;
    ucm::File::readTextFile(path, json);
    if (json.length() == 0) return false;

    ucm::JSONReader reader(json);
    ucm::JSObject* obj = reader.readObject();
    if (!obj) return false;

    obj->tryGetNumberProperty("samples",          &params.samples);
    obj->tryGetNumberProperty("threads",          &params.threads);
    if (obj->hasProperty("denoise"))
        params.denoise = obj->isBooleanPropertyTrue("denoise");
    obj->tryGetNumberProperty("denoiseIntensity", &params.denoiseIntensity);
    // Camera
    readVec3Into(obj, "location", params.camLocation);
    readVec3Into(obj, "angle",    params.camAngle);
    obj->tryGetNumberProperty("fieldOfView",      &params.fieldOfView);
    obj->tryGetNumberProperty("depthOfField",     &params.depthOfField);
    obj->tryGetNumberProperty("aperture",         &params.aperture);
    obj->tryGetNumberProperty("apertureBlades",   &params.apertureBlades);
    obj->tryGetNumberProperty("apertureRotation", &params.apertureRotation);
    obj->tryGetNumberProperty("exposure",         &params.exposure);
    // Scene
    obj->tryGetNumberProperty("envIntensity",     &params.envIntensity);
    obj->tryGetNumberProperty("envRotation",      &params.envRotation);
    if (obj->hasProperty("mediumEnabled"))
        params.mediumEnabled = obj->isBooleanPropertyTrue("mediumEnabled");
    readVec3Into(obj, "mediumSigmaA",   params.mediumSigmaA);
    readVec3Into(obj, "mediumSigmaS",   params.mediumSigmaS);
    readVec3Into(obj, "mediumEmission", params.mediumEmission);
    obj->tryGetNumberProperty("mediumG",       &params.mediumG);
    obj->tryGetNumberProperty("mediumDensity", &params.mediumDensity);
    // Post-process
    if (obj->hasProperty("postProcess"))
        params.postProcess = obj->isBooleanPropertyTrue("postProcess");
    obj->tryGetNumberProperty("bloomThreshold",   &params.bloomThreshold);
    obj->tryGetNumberProperty("bloomStrength",    &params.bloomStrength);
    obj->tryGetNumberProperty("bloomCurve",       &params.bloomCurve);
    obj->tryGetNumberProperty("bloomRadius",      &params.bloomRadius);
    // Output
    obj->tryGetNumberProperty("outputWidth",      &outputWidth);
    obj->tryGetNumberProperty("outputHeight",     &outputHeight);

    delete obj;
    return true;
}

static void saveViewerConfig(const char* path, const ViewerParams& params,
                             int outputWidth, int outputHeight) {
    ucm::JSONWriter w;
    w.beginObject();
    w.writeProperty("samples",          (int)params.samples);
    w.writeProperty("threads",          (int)params.threads);
    w.writeProperty("denoise",          params.denoise);
    w.writeProperty("denoiseIntensity", (double)params.denoiseIntensity);
    writeVec3(w,     "location",        params.camLocation);
    writeVec3(w,     "angle",           params.camAngle);
    w.writeProperty("fieldOfView",      (double)params.fieldOfView);
    w.writeProperty("depthOfField",     (double)params.depthOfField);
    w.writeProperty("aperture",         (double)params.aperture);
    w.writeProperty("apertureBlades",   (int)params.apertureBlades);
    w.writeProperty("apertureRotation", (double)params.apertureRotation);
    w.writeProperty("exposure",         (double)params.exposure);
    w.writeProperty("envIntensity",     (double)params.envIntensity);
    w.writeProperty("envRotation",      (double)params.envRotation);
    w.writeProperty("mediumEnabled",    params.mediumEnabled);
    writeVec3(w,    "mediumSigmaA",     params.mediumSigmaA);
    writeVec3(w,    "mediumSigmaS",     params.mediumSigmaS);
    writeVec3(w,    "mediumEmission",   params.mediumEmission);
    w.writeProperty("mediumG",          (double)params.mediumG);
    w.writeProperty("mediumDensity",    (double)params.mediumDensity);
    w.writeProperty("postProcess",      params.postProcess);
    w.writeProperty("bloomThreshold",   (double)params.bloomThreshold);
    w.writeProperty("bloomStrength",    (double)params.bloomStrength);
    w.writeProperty("bloomCurve",       (double)params.bloomCurve);
    w.writeProperty("bloomRadius",      (double)params.bloomRadius);
    w.writeProperty("outputWidth",      outputWidth);
    w.writeProperty("outputHeight",     outputHeight);
    w.endObject();
    ucm::File::writeTextFile(path, w.getString().getBuffer());
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
    s.threads                   = p.threads;
    s.enableDenoise             = p.denoise;
    s.denoiseIntensity          = p.denoiseIntensity;
    s.enableRenderingPostProcess = p.postProcess;
    s.bloomThreshold            = p.bloomThreshold;
    s.bloomStrength             = p.bloomStrength;
    s.bloomCurve                = p.bloomCurve;
    s.bloomRadius               = p.bloomRadius;

    scene.envmapIntensity = p.envIntensity;
    scene.envmapRotation  = p.envRotation;

    // Global medium: lazily heap-alloc on first enable, mutate in place after
    // that so we don't churn the allocator on every slider tick. When the user
    // disables it we keep the allocation but zero the σ values — `isActive()`
    // gates the volumetric branch on σt_hero > 0, so the renderer falls back
    // to the legacy fast path automatically. (Prepare() recomputes σt_hero.)
    if (p.mediumEnabled) {
        if (scene.globalMedium == NULL) scene.globalMedium = new HomogeneousMedium();
        HomogeneousMedium* m = scene.globalMedium;
        m->sigma_a  = color3(p.mediumSigmaA[0], p.mediumSigmaA[1], p.mediumSigmaA[2]);
        m->sigma_s  = color3(p.mediumSigmaS[0], p.mediumSigmaS[1], p.mediumSigmaS[2]);
        m->sigma_e  = color3(p.mediumEmission[0], p.mediumEmission[1], p.mediumEmission[2]);
        m->g        = p.mediumG;
        m->density  = p.mediumDensity;
        m->prepare();
    } else if (scene.globalMedium != NULL) {
        scene.globalMedium->sigma_a = color3::zero;
        scene.globalMedium->sigma_s = color3::zero;
        scene.globalMedium->sigma_e = color3::zero;
        scene.globalMedium->prepare();
    }

    if (scene.mainCamera) {
        Camera& cam = *scene.mainCamera;
        cam.location         = vec3(p.camLocation[0], p.camLocation[1], p.camLocation[2]);
        cam.angle            = vec3(p.camAngle[0],    p.camAngle[1],    p.camAngle[2]);
        cam.fieldOfView      = p.fieldOfView;
        cam.depthOfField     = p.depthOfField;
        cam.aperture         = p.aperture;
        cam.apertureBlades   = p.apertureBlades;
        cam.apertureRotation = p.apertureRotation;
        cam.exposure         = p.exposure;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <scene.json>\n", argv[0]);
        return 1;
    }
    const char* scenePath = argv[1];

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

    // Two independent scales:
    //   fontPixelScale — HiDPI rasterization factor so text stays crisp on
    //     Retina / fractional-DPI displays. Always driven by the OS.
    //   widgetScale    — ImGui widget zoom.
    //     On macOS the GLFW backend reports DisplaySize in POINTS and
    //     DisplayFramebufferScale = contentScale, so ImGui's 96-DPI-tuned
    //     defaults end up physically 2x on Retina. Cancel the point→pixel
    //     blow-up with widgetScale = 1/contentScale.
    //     On Windows the backend reports DisplaySize in pixels and doesn't
    //     auto-scale, so we mirror the OS content scale onto widgets.
    //     RAYGEN_UI_SCALE overrides it end-to-end, e.g. `RAYGEN_UI_SCALE=0.9`.
    float fontPixelScale = 1.0f;
    float widgetScale    = 1.0f;
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(window, &xs, &ys);
        fontPixelScale = xs;
#ifdef __APPLE__
        widgetScale = (xs > 0.0f) ? (1.0f / xs) : 1.0f;
#else
        widgetScale = xs;
#endif
        if (const char* env = getenv("RAYGEN_UI_SCALE")) {
            float v = (float)atof(env);
            if (v >= 0.25f && v <= 4.0f) widgetScale = v;
        }
        if (widgetScale < 0.25f) widgetScale = 0.25f;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Crisp text: rasterize the default font at the combined pixel size
    // (widget zoom × HiDPI) rather than relying on FontGlobalScale, which
    // bilinear-stretches a small atlas.
    {
        ImFontConfig cfg;
        cfg.SizePixels = 13.0f * widgetScale * fontPixelScale;
        io.Fonts->AddFontDefault(&cfg);
    }
    ImGui::GetStyle().ScaleAllSizes(widgetScale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Scene + renderer live here on the main thread; the worker only touches
    // them while it holds the job mutex. That's enough for a cooperative
    // single-render-at-a-time model - no in-flight render is mutated.
    RendererSettings rsInit;
    rsInit.resolutionWidth  = 600;
    rsInit.resolutionHeight = 375;
    RayRenderer renderer(&rsInit);

    // unique_ptr lets us swap the Scene object on reload without disturbing
    // the worker's capture - the worker dereferences `*scene` each job, so a
    // pointer-swap from the main thread (while the worker is idle) is safe.
    auto scene = std::make_unique<Scene>();
    {
        RendererSceneLoader loader;
        loader.load(renderer, scene.get(), scenePath);
    }
    renderer.setScene(scene.get());

    // Seed UI params from the just-loaded scene/renderer so sliders start
    // where the scene.json (and RendererSettings defaults) left off.
    auto seedUiParamsFromScene = [&](ViewerParams& p) {
        p.samples          = renderer.settings.samples;
        p.threads          = renderer.settings.threads;
        p.denoise          = renderer.settings.enableDenoise;
        p.denoiseIntensity = renderer.settings.denoiseIntensity;
        p.postProcess      = renderer.settings.enableRenderingPostProcess;
        p.bloomThreshold   = renderer.settings.bloomThreshold;
        p.bloomStrength    = renderer.settings.bloomStrength;
        p.bloomCurve       = renderer.settings.bloomCurve;
        p.bloomRadius      = renderer.settings.bloomRadius;
        p.envIntensity     = scene->envmapIntensity;
        p.envRotation      = scene->envmapRotation;
        if (scene->globalMedium != NULL) {
            const HomogeneousMedium* m = scene->globalMedium;
            p.mediumEnabled    = m->isActive() || (m->sigma_e_eff != color3::zero);
            p.mediumSigmaA[0]  = m->sigma_a.r;
            p.mediumSigmaA[1]  = m->sigma_a.g;
            p.mediumSigmaA[2]  = m->sigma_a.b;
            p.mediumSigmaS[0]  = m->sigma_s.r;
            p.mediumSigmaS[1]  = m->sigma_s.g;
            p.mediumSigmaS[2]  = m->sigma_s.b;
            p.mediumEmission[0] = m->sigma_e.r;
            p.mediumEmission[1] = m->sigma_e.g;
            p.mediumEmission[2] = m->sigma_e.b;
            p.mediumG          = m->g;
            p.mediumDensity    = m->density;
        }
        if (scene->mainCamera) {
            const Camera& cam = *scene->mainCamera;
            p.camLocation[0]   = cam.location.x;
            p.camLocation[1]   = cam.location.y;
            p.camLocation[2]   = cam.location.z;
            p.camAngle[0]      = cam.angle.x;
            p.camAngle[1]      = cam.angle.y;
            p.camAngle[2]      = cam.angle.z;
            p.fieldOfView      = cam.fieldOfView;
            p.depthOfField     = cam.depthOfField;
            p.aperture         = cam.aperture;
            p.apertureBlades   = cam.apertureBlades;
            p.apertureRotation = cam.apertureRotation;
            p.exposure         = cam.exposure;
        }
    };

    ViewerParams uiParams;
    seedUiParamsFromScene(uiParams);

    // Build a default output path next to the scene.json: replace the
    // extension with "-out.jpg" so hitting Save with no edits still produces
    // something sensible.
    char outputPath[512] = {0};
    {
        const char* dot = strrchr(scenePath, '.');
        size_t baseLen = dot ? (size_t)(dot - scenePath) : strlen(scenePath);
        if (baseLen > sizeof(outputPath) - 16) baseLen = sizeof(outputPath) - 16;
        memcpy(outputPath, scenePath, baseLen);
        memcpy(outputPath + baseLen, "-out.jpg", 9);
    }

    // Sidecar "<scene>.viewer.json": auto-loaded here (scene-seeded values
    // act as fallbacks for missing keys) and auto-saved on every render kick
    // further down. Lets us persist slider state across sessions without
    // touching scene.json (which preserves JSONC comments and author intent).
    char sidecarPath[512] = {0};
    computeSidecarPath(scenePath, sidecarPath, sizeof(sidecarPath));
    int sidecarOutW = renderer.settings.resolutionWidth;
    int sidecarOutH = renderer.settings.resolutionHeight;
    const bool sidecarLoaded =
        loadViewerConfig(sidecarPath, uiParams, sidecarOutW, sidecarOutH);
    if (sidecarLoaded) {
        renderer.settings.resolutionWidth  = sidecarOutW;
        renderer.settings.resolutionHeight = sidecarOutH;
        renderer.setRenderSize(sidecarOutW, sidecarOutH);
    }

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
            applyParamsToScene(p, renderer, *scene);

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

                    {
                        std::lock_guard<std::mutex> lk(job.mu);
                        job.result.rgba = std::move(rgba);
                        job.result.width = w;
                        job.result.height = h;
                        job.result.secs = now;
                        job.result.isPreview = true;
                        job.result.previewProgress = progress;
                        job.ready = true;
                    }
                    // Wake the main thread (which is blocked in
                    // glfwWaitEventsTimeout) so the partial frame shows up
                    // without waiting for the next input event.
                    glfwPostEmptyEvent();
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
            glfwPostEmptyEvent();
        }
    });

    // Worker hand-off. Kept narrow (no file I/O under the mutex) so the
    // render thread picks up the pending params as soon as it's woken.
    auto enqueueWorker = [&](const ViewerParams& p, JobKind kind) {
        std::lock_guard<std::mutex> lk(job.mu);
        job.pending = p;
        job.pendingKind = kind;
        job.hasPending = true;
        job.cv.notify_one();
    };

    // Persist the user-facing ViewerParams (not any preview-time override
    // like samples=1) to the sidecar. File write happens outside the worker
    // mutex so a slow disk can't stall the handoff.
    auto persistSidecar = [&]() {
        saveViewerConfig(sidecarPath, uiParams,
                         renderer.settings.resolutionWidth,
                         renderer.settings.resolutionHeight);
    };

    // Convenience: fire a full-quality kick and persist. Used for initial
    // load and explicit user actions (Re-render button, Apply, Reload).
    auto kickFinal = [&](JobKind kind = JobKind::Full) {
        enqueueWorker(uiParams, kind);
        persistSidecar();
    };
    kickFinal();

    GLuint renderTex = 0;
    int   texW = 0, texH = 0;
    double lastRenderSec = 0.0;
    bool  isRendering = true;
    JobKind currentJobKind = JobKind::Full;
    float previewProgress = 0.0f;
    bool  lastUploadWasPreview = false;
    ViewerParams lastKickedParams = uiParams;

    // Output resolution: deliberately kept out of ViewerParams so slider
    // motion never resizes the buffer mid-drag. The user commits changes by
    // clicking Apply, which must happen while the worker is idle.
    int outputWidth  = renderer.settings.resolutionWidth;
    int outputHeight = renderer.settings.resolutionHeight;

    // Preview image transform. `imageZoom` is a 2D scale; `imagePan` is the
    // offset of the image centre from the canvas centre (screen pixels).
    // Wheel-zoom preserves the image-space point under the cursor; drag on
    // the canvas translates `imagePan`.
    float  imageZoom = 1.0f;
    ImVec2 imagePan  = ImVec2(0.0f, 0.0f);

    // Currently selected scene object (for the Property window). Lifetime is
    // the current Scene's — Reload swaps the Scene unique_ptr so we clear the
    // selection there to avoid a dangling pointer.
    SceneObject* selectedObj = nullptr;

    // Set whenever a slider/edit has changed something relative to the last
    // kicked params (either ViewerParams via the control panel, or Scene
    // state via Outline/Property). The single kick site in the control panel
    // fires once per frame when the worker goes idle. Hoisted out of the
    // control-panel scope so the Outline/Property code below can drive it.
    bool pendingDirty = false;

    // Recursive draw for the Outline tree. Any mutation of visibility or
    // material happens directly on the SceneObject — ImGui edit widgets return
    // true whenever the user dragged, which drives `dirty`. The tree is drawn
    // every frame so new children (post-Reload) appear automatically.
    std::function<void(SceneObject*, bool&)> drawObjectNode;
    drawObjectNode = [&](SceneObject* obj, bool& outDirty) {
        if (!obj) return;
        const auto& children = obj->getObjects();
        const bool leaf = children.empty();

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (obj == selectedObj) flags |= ImGuiTreeNodeFlags_Selected;

        // Visibility checkbox on the left; ##<addr> keeps IDs unique even when
        // two siblings share a display name.
        ImGui::PushID((void*)obj);
        bool visible = obj->visible;
        if (ImGui::Checkbox("##vis", &visible)) {
            obj->visible = visible;
            outDirty = true;
        }
        ImGui::SameLine();

        const char* label = obj->getName().isEmpty() ? "(unnamed)"
                                                     : obj->getName().getBuffer();
        bool opened = ImGui::TreeNodeEx((void*)obj, flags, "%s", label);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selectedObj = obj;
        }
        ImGui::PopID();

        if (opened && !leaf) {
            for (SceneObject* child : children) drawObjectNode(child, outDirty);
            ImGui::TreePop();
        }
    };

    while (!glfwWindowShouldClose(window)) {
        // Event-driven: sleep until the OS or the worker (via
        // glfwPostEmptyEvent) gives us something to do. A short timeout
        // while a render is in-flight lets the progress bar tick between
        // worker snapshots; a longer one at rest keeps the viewer truly
        // idle (no 120fps redraw burn on battery).
        glfwWaitEventsTimeout(isRendering ? 0.05 : 0.5);

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
                ImGui::SameLine();
                // Cooperative cancel: the renderer checks cancelRequested at
                // every row boundary. Clicking x here returns within a few ms.
                if (ImGui::SmallButton("x##cancel")) {
                    renderer.cancelRequested = true;
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("cancel this render");
                ImGui::ProgressBar(previewProgress, ImVec2(-1, 4));
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1.0f), "status: ready");
            ImGui::ProgressBar(1.0f, ImVec2(-1, 4));
        }

        bool dirty = false;

        if (ImGui::CollapsingHeader("Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::SliderInt("samples", &uiParams.samples, 1, 1000);
            dirty |= ImGui::SliderInt("threads", &uiParams.threads, 1, 32);
            dirty |= ImGui::Checkbox ("denoise", &uiParams.denoise);
            if (uiParams.denoise) {
                dirty |= ImGui::SliderFloat("denoise intensity", &uiParams.denoiseIntensity, 0.0f, 1.0f, "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Drag widgets for position/rotation so a big scene (e.g. far-away
            // mesh) and a small scene (tabletop) both feel natural; units are
            // world-space metres for location, degrees for angle.
            dirty |= ImGui::DragFloat3("location",         uiParams.camLocation,  0.05f, -1000.0f, 1000.0f, "%.3f");
            dirty |= ImGui::DragFloat3("angle",            uiParams.camAngle,     0.5f,  -360.0f, 360.0f,   "%.2f");
            dirty |= ImGui::SliderFloat("fieldOfView",    &uiParams.fieldOfView,  10.0f,  120.0f,           "%.1f");
            dirty |= ImGui::SliderFloat("depthOfField",   &uiParams.depthOfField,  0.0f,  50.0f,            "%.2f");
            // Show a hint when the scene pinned focus to an object: the
            // renderer overwrites depthOfField each frame from that object's
            // bbox, so the slider's value won't stick until it's cleared.
            if (scene->mainCamera && !scene->mainCamera->focusOnObjectName.isEmpty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("(focus lock: %s)",
                                    scene->mainCamera->focusOnObjectName.getBuffer());
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##focus")) {
                    scene->mainCamera->focusOnObjectName.clear();
                    dirty = true;
                }
            }
            // aperture=0 disables DOF entirely (the renderer gates on
            // ctx.aperture > 0), so keeping 0 reachable lets the user
            // get pinhole-sharp output without touching depthOfField.
            dirty |= ImGui::SliderFloat("aperture",       &uiParams.aperture,      0.0f,  22.0f, "f/%.2f");
            dirty |= ImGui::SliderInt  ("apertureBlades", &uiParams.apertureBlades, 0,    12);
            dirty |= ImGui::SliderFloat("apertureRotation", &uiParams.apertureRotation, 0.0f, 360.0f, "%.1f");
            dirty |= ImGui::SliderFloat("exposure",       &uiParams.exposure,      0.1f,  3.0f, "%.2f");
        }

        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::SliderFloat("envmap intensity", &uiParams.envIntensity, 0.0f, 3.0f, "%.2f");
            dirty |= ImGui::SliderFloat("envmap rotation",  &uiParams.envRotation,  0.0f, 360.0f, "%.0f");

            // Global medium (fog). σa/σs/σe are per-channel inverse world-units;
            // density is a uniform multiplier that prepare() folds into σt_hero
            // for free-flight sampling. g is the Henyey-Greenstein anisotropy
            // (0 = isotropic, >0 = forward-peaked clouds/fog, <0 = back-scatter).
            ImGui::Spacing();
            ImGui::TextDisabled("Global medium (fog)");
            dirty |= ImGui::Checkbox("enable##medium", &uiParams.mediumEnabled);
            if (uiParams.mediumEnabled) {
                dirty |= ImGui::DragFloat3 ("sigma_a",  uiParams.mediumSigmaA,   0.005f, 0.0f, 10.0f, "%.4f");
                dirty |= ImGui::DragFloat3 ("sigma_s",  uiParams.mediumSigmaS,   0.005f, 0.0f, 10.0f, "%.4f");
                dirty |= ImGui::DragFloat3 ("emission", uiParams.mediumEmission, 0.05f,  0.0f, 100.0f, "%.3f");
                dirty |= ImGui::SliderFloat("g (HG)",  &uiParams.mediumG,       -0.95f,  0.95f, "%.2f");
                dirty |= ImGui::SliderFloat("density", &uiParams.mediumDensity,  0.0f,   8.0f,  "%.2f");
            }
        }

        if (ImGui::CollapsingHeader("Post-process (bloom)", ImGuiTreeNodeFlags_DefaultOpen)) {
            dirty |= ImGui::Checkbox("enable##pp", &uiParams.postProcess);
            if (uiParams.postProcess) {
                dirty |= ImGui::SliderFloat("bloom threshold", &uiParams.bloomThreshold, 0.0f,  2.0f, "%.2f");
                dirty |= ImGui::SliderFloat("bloom strength",  &uiParams.bloomStrength,  0.0f,  5.0f,  "%.2f");
                dirty |= ImGui::SliderFloat("bloom curve",     &uiParams.bloomCurve,     1.0f,  4.0f,  "%.2f");
                dirty |= ImGui::SliderFloat("bloom radius",    &uiParams.bloomRadius,    0.0f,  0.15f, "%.3f");
            }
            ImGui::TextDisabled("TODO: post-process-only re-run once the core\n"
                                "caches a pre-PP image");
        }

        ImGui::Separator();
        const bool canKick = !isRendering;

        // Intent tracking: `pendingDirty` (declared above in fn scope) is
        // set whenever sliders move while we can't kick, and the single kick
        // site below fires at most once per frame. The previous code fired
        // both an immediate kick *and* the pending kick in the same frame
        // after a PostOnly finished, which left the second kick comparing
        // lastKickedParams (just updated) against itself, reporting "no bloom
        // diff", and falling back to JobKind::Full - a full re-trace with
        // fresh noise.
        if (dirty) pendingDirty = true;

        if (!canKick) ImGui::BeginDisabled();
        if (ImGui::Button("Re-render (full)")) {
            lastKickedParams = uiParams;
            kickFinal(JobKind::Full);
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

        // The auto-kick decision runs at end-of-frame so it catches widget
        // activity from every panel (Outline, Property, etc.) — not just the
        // control panel drawn first. See below, just before ImGui::Render().

        ImGui::End();

        // --- Output resolution window ---
        ImGui::Begin("Output");
        ImGui::InputInt("width",  &outputWidth,  10, 100);
        ImGui::InputInt("height", &outputHeight, 10, 100);
        if (outputWidth  < 16)   outputWidth  = 16;
        if (outputHeight < 16)   outputHeight = 16;
        if (outputWidth  > 8192) outputWidth  = 8192;
        if (outputHeight > 8192) outputHeight = 8192;

        const bool sizeChanged =
            outputWidth  != renderer.settings.resolutionWidth ||
            outputHeight != renderer.settings.resolutionHeight;
        const bool canApply = !isRendering && sizeChanged;

        // Shared helper: commit current outputWidth/Height and kick a full
        // render. Safe to call only while the worker is idle.
        auto applyResolution = [&]() {
            renderer.settings.resolutionWidth  = outputWidth;
            renderer.settings.resolutionHeight = outputHeight;
            renderer.setRenderSize(outputWidth, outputHeight);
            lastKickedParams = uiParams;
            kickFinal(JobKind::Full);
        };

        if (!canApply) ImGui::BeginDisabled();
        if (ImGui::Button("Apply")) applyResolution();
        if (!canApply) ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("current: %d x %d",
                            renderer.settings.resolutionWidth,
                            renderer.settings.resolutionHeight);

        // Presets now auto-apply: one click jumps to the resolution and kicks
        // a render, as long as the worker is idle. If it's busy, the button
        // is disabled so we don't race with setRenderSize.
        const bool canPreset = !isRendering;
        auto preset = [&](int w, int h) {
            outputWidth = w; outputHeight = h;
            if (canPreset) applyResolution();
        };
        ImGui::Text("preset:");
        ImGui::SameLine();
        if (!canPreset) ImGui::BeginDisabled();
        if (ImGui::SmallButton("480p"))  preset(854,  480);
        ImGui::SameLine();
        if (ImGui::SmallButton("720p"))  preset(1280, 720);
        ImGui::SameLine();
        if (ImGui::SmallButton("1080p")) preset(1920, 1080);
        ImGui::SameLine();
        if (ImGui::SmallButton("2K"))    preset(2560, 1440);
        ImGui::SameLine();
        if (ImGui::SmallButton("4K"))    preset(3840, 2160);
        if (!canPreset) ImGui::EndDisabled();
        ImGui::End();

        // --- File window (save / reload) ---
        ImGui::Begin("File");
        ImGui::TextWrapped("scene: %s", scenePath);
        const bool canFile = !isRendering;
        if (!canFile) ImGui::BeginDisabled();
        if (ImGui::Button("Reload scene")) {
            // Drop the old Scene (destructor releases meshes) and re-parse
            // the JSON into a fresh one. Worker sees the new *scene on the
            // next job because it dereferences the unique_ptr each time.
            // Clear the selection first — the old SceneObject* dangles once
            // the unique_ptr swap runs.
            selectedObj = nullptr;
            auto fresh = std::make_unique<Scene>();
            RendererSceneLoader loader2;
            loader2.load(renderer, fresh.get(), scenePath);
            renderer.setScene(fresh.get());
            scene = std::move(fresh);

            // Re-seed from scene then overlay the sidecar, so Reload behaves
            // symmetrically with initial load: user tweaks win, scene defaults
            // fill in the rest.
            seedUiParamsFromScene(uiParams);
            int reloadOutW = renderer.settings.resolutionWidth;
            int reloadOutH = renderer.settings.resolutionHeight;
            if (loadViewerConfig(sidecarPath, uiParams, reloadOutW, reloadOutH)) {
                renderer.settings.resolutionWidth  = reloadOutW;
                renderer.settings.resolutionHeight = reloadOutH;
                renderer.setRenderSize(reloadOutW, reloadOutH);
                outputWidth  = reloadOutW;
                outputHeight = reloadOutH;
            }

            lastKickedParams = uiParams;
            kickFinal(JobKind::Full);
        }
        if (!canFile) ImGui::EndDisabled();

        ImGui::InputText("output path", outputPath, sizeof(outputPath));
        const bool canSave = !isRendering && renderTex != 0 && outputPath[0] != 0;
        if (!canSave) ImGui::BeginDisabled();
        if (ImGui::Button("Save render")) {
            // .hdr → linear-radiance HDR buffer (float, no tonemap).
            // Anything else → tonemapped LDR preview. saveImage routes by
            // extension via getImageFormatByExtension.
            ucm::string outPath(outputPath);
            ImageCodecFormat outFmt = ImageCodecFormat::ICF_AUTO;
            getImageFormatByExtension(outPath, &outFmt);
            if (outFmt == ImageCodecFormat::ICF_HDR) {
                saveImage(renderer.getHdrResult(), outPath);
            } else {
                saveImage(renderer.getRenderResult(), outPath);
            }
        }
        if (!canSave) ImGui::EndDisabled();
        ImGui::End();

        // --- Render preview ---
        // The image is drawn as a free-floating quad inside a fixed-size
        // canvas (not via ImGui::Image + scrollbars). This lets wheel-zoom
        // preserve the image-space point under the cursor, and lets left-
        // drag translate the image — the window chrome itself doesn't move.
        ImGui::Begin("render", nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        if (renderTex != 0) {
            ImGui::Text("zoom: %.2fx", imageZoom);
            ImGui::SameLine();
            if (ImGui::SmallButton("1:1")) {
                imageZoom = 1.0f;
                imagePan  = ImVec2(0.0f, 0.0f);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("fit")) {
                const ImVec2 avail = ImGui::GetContentRegionAvail();
                const float sx = avail.x / (float)texW;
                const float sy = avail.y / (float)texH;
                imageZoom = (sx < sy) ? sx : sy;
                if (imageZoom < 0.05f) imageZoom = 0.05f;
                imagePan = ImVec2(0.0f, 0.0f);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(wheel = zoom, drag = pan)");

            // Canvas area fills the rest of the window. An invisible button
            // captures left/middle-click so dragging inside stays on the
            // image (not the ImGui window).
            const ImVec2 canvasStart = ImGui::GetCursorScreenPos();
            const ImVec2 canvasSize  = ImGui::GetContentRegionAvail();
            if (canvasSize.x > 0.0f && canvasSize.y > 0.0f) {
                const ImVec2 canvasCenter(canvasStart.x + canvasSize.x * 0.5f,
                                          canvasStart.y + canvasSize.y * 0.5f);

                ImGui::InvisibleButton("##canvas", canvasSize,
                                       ImGuiButtonFlags_MouseButtonLeft |
                                       ImGuiButtonFlags_MouseButtonMiddle);
                const bool canvasHovered = ImGui::IsItemHovered();
                const bool canvasActive  = ImGui::IsItemActive();

                // Wheel zoom toward mouse cursor. Derivation: keep the image-
                // space point under the cursor fixed while scaling, i.e.
                //   pan' = (1 - k) * (mouse - center) + k * pan
                // where k = zoomNew / zoomOld.
                if (canvasHovered && io.MouseWheel != 0.0f) {
                    const float oldZoom = imageZoom;
                    float newZoom = oldZoom * (io.MouseWheel > 0.0f ? 1.1f : (1.0f / 1.1f));
                    if (newZoom < 0.05f) newZoom = 0.05f;
                    if (newZoom > 16.0f) newZoom = 16.0f;
                    if (newZoom != oldZoom) {
                        const float k = newZoom / oldZoom;
                        const float dmx = io.MousePos.x - canvasCenter.x;
                        const float dmy = io.MousePos.y - canvasCenter.y;
                        imagePan.x = (1.0f - k) * dmx + k * imagePan.x;
                        imagePan.y = (1.0f - k) * dmy + k * imagePan.y;
                        imageZoom = newZoom;
                    }
                }

                // Drag-to-pan (left button, cursor over canvas).
                if (canvasActive && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                    imagePan.x += io.MouseDelta.x;
                    imagePan.y += io.MouseDelta.y;
                }

                // Clip the image to the canvas rect so it doesn't bleed into
                // the title-bar region or neighbouring panels.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 clipMax(canvasStart.x + canvasSize.x,
                                     canvasStart.y + canvasSize.y);
                dl->PushClipRect(canvasStart, clipMax, true);

                const ImVec2 displaySize(texW * imageZoom, texH * imageZoom);
                const ImVec2 imgMin(canvasCenter.x - displaySize.x * 0.5f + imagePan.x,
                                    canvasCenter.y - displaySize.y * 0.5f + imagePan.y);
                const ImVec2 imgMax(imgMin.x + displaySize.x,
                                    imgMin.y + displaySize.y);
                dl->AddImage((ImTextureID)(intptr_t)renderTex, imgMin, imgMax);

                dl->PopClipRect();
            }
        } else {
            ImGui::Text("(warming up...)");
        }
        ImGui::End();

        // --- Outline window (scene tree) ---
        // Edits are live even during rendering — same model as the main
        // control panel. `transformScene()` snapshots triangles + BVH at the
        // start of each render(), so mid-render visibility/transform changes
        // don't affect the current frame; they're picked up by the next Full
        // kick fired when the worker goes idle. Per-hit material reads can
        // briefly mix old/new channels on a dragging slider, but aligned 32-
        // bit float writes are atomic so there's no corruption — only a few
        // transient pixels that the next render cleans up.
        bool sceneDirty = false;
        ImGui::Begin("Outline");
        for (SceneObject* root : scene->getObjects()) {
            drawObjectNode(root, sceneDirty);
        }
        ImGui::End();

        // --- Property window (inspector for selectedObj) ---
        ImGui::Begin("Property");
        if (!selectedObj) {
            ImGui::TextDisabled("Select an object in the Outline window to inspect.");
        } else {
            SceneObject* so = selectedObj;
            ImGui::Text("name: %s",
                        so->getName().isEmpty() ? "(unnamed)"
                                                : so->getName().getBuffer());
            ImGui::Text("meshes: %zu   children: %zu",
                        so->getMeshes().size(), so->getObjects().size());
            ImGui::Separator();

            // Transform — edits apply instantly to the SceneObject. The
            // renderer re-flattens + rebuilds the BVH on every Full render, so
            // a dirty kick is enough to see the change.
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                float loc[3] = { so->location.x, so->location.y, so->location.z };
                float ang[3] = { so->angle.x,    so->angle.y,    so->angle.z };
                float scl[3] = { so->scale.x,    so->scale.y,    so->scale.z };
                if (ImGui::DragFloat3("location", loc, 0.05f, -1000.0f, 1000.0f, "%.3f")) {
                    so->location = vec3(loc[0], loc[1], loc[2]);
                    sceneDirty = true;
                }
                if (ImGui::DragFloat3("angle",    ang, 0.5f,  -360.0f, 360.0f, "%.2f")) {
                    so->angle = vec3(ang[0], ang[1], ang[2]);
                    sceneDirty = true;
                }
                if (ImGui::DragFloat3("scale",    scl, 0.01f, 0.0001f, 1000.0f, "%.3f")) {
                    so->scale = vec3(scl[0], scl[1], scl[2]);
                    sceneDirty = true;
                }

                bool v = so->visible;
                if (ImGui::Checkbox("visible", &v))    { so->visible = v;    sceneDirty = true; }
                ImGui::SameLine();
                bool r = so->renderable;
                if (ImGui::Checkbox("renderable", &r)) { so->renderable = r; sceneDirty = true; }
            }

            // Material — edits write directly to SceneObject::material. The
            // renderer reads material by value per-hit via a pointer on the
            // triangle, so the change shows up on the next trace.
            if (ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                Material& m = so->material;
                if (!m.name.isEmpty()) {
                    ImGui::TextDisabled("shared: %s", m.name.getBuffer());
                }

                float col[3] = { m.color.r, m.color.g, m.color.b };
                if (ImGui::ColorEdit3("color", col)) {
                    m.color = color3(col[0], col[1], col[2]);
                    sceneDirty = true;
                }
                sceneDirty |= ImGui::SliderFloat("diffuse",          &m.diffuse,          0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("glossy",           &m.glossy,           0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("metallic",         &m.metallic,         0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("roughness",        &m.roughness,        0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("anisotropy",       &m.anisotropy,      -1.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("anisoRotation",    &m.anisoRotation,    0.0f, 360.0f, "%.1f");
                sceneDirty |= ImGui::SliderFloat("transparency",     &m.transparency,     0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("refraction",       &m.refraction,       0.0f, 1.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("refractionRatio",  &m.refractionRatio,  1.0f, 3.0f, "%.3f");
                sceneDirty |= ImGui::SliderFloat("chromaDispersion", &m.chromaDispersion, 0.0f, 0.1f, "%.4f");
                sceneDirty |= ImGui::DragFloat  ("emission",         &m.emission,         0.1f, 0.0f, 10000.0f, "%.2f");
                sceneDirty |= ImGui::DragFloat  ("spotRange",        &m.spotRange,        0.01f, 0.0f, 100.0f, "%.3f");

                if (!m.texturePath.isEmpty())
                    ImGui::TextDisabled("texture:    %s", m.texturePath.getBuffer());
                if (!m.normalmapPath.isEmpty())
                    ImGui::TextDisabled("normal map: %s", m.normalmapPath.getBuffer());
            }

            // Interior medium — participating volume that fills the object.
            // Enabled state is "the SceneObject owns a HomogeneousMedium",
            // not derived from σ values: pulling density (or every σ) to 0
            // would otherwise flip the checkbox off and hide the sliders, so
            // the user couldn't dial it back up. Disable deletes the medium;
            // re-enable allocates a fresh zero-init one.
            if (ImGui::CollapsingHeader("Interior medium")) {
                HomogeneousMedium* m = so->interiorMedium;
                bool enabled = (m != NULL);
                if (ImGui::Checkbox("enable##interiorMedium", &enabled)) {
                    if (enabled && m == NULL) {
                        so->interiorMedium = new HomogeneousMedium();
                        // Default newly-created media to follow the object so
                        // dragging the bounding mesh in the Property panel
                        // also moves the flame. Existing JSON-loaded media
                        // keep whatever the file authored.
                        so->interiorMedium->coneFollowObject = true;
                        so->interiorMedium->prepare();
                    } else if (!enabled && m != NULL) {
                        delete m;
                        so->interiorMedium = NULL;
                    }
                    sceneDirty = true;
                }
                if (enabled && (m = so->interiorMedium) != NULL) {
                    float sa[3] = { m->sigma_a.r, m->sigma_a.g, m->sigma_a.b };
                    float ss[3] = { m->sigma_s.r, m->sigma_s.g, m->sigma_s.b };
                    float se[3] = { m->sigma_e.r, m->sigma_e.g, m->sigma_e.b };
                    bool changed = false;
                    changed |= ImGui::DragFloat3 ("sigma_a##im",  sa, 0.01f, 0.0f, 20.0f, "%.4f");
                    changed |= ImGui::DragFloat3 ("sigma_s##im",  ss, 0.01f, 0.0f, 20.0f, "%.4f");
                    changed |= ImGui::SliderFloat("g (HG)##im",  &m->g,       -0.95f, 0.95f, "%.2f");
                    changed |= ImGui::SliderFloat("density##im", &m->density,  0.0f,  8.0f,  "%.2f");

                    // Emission mode picker. Constant uses the analytic σe
                    // integral (sigma_e slider). Cone evaluates a procedural
                    // jet-flame profile sampled along the ray — flipping the
                    // mode hides/shows the relevant slider set so the panel
                    // doesn't sprout dead inputs.
                    const char* modeLabels[] = { "Constant", "Cone (jet flame)" };
                    int modeIdx = (int)m->emissionMode;
                    if (ImGui::Combo("emissionMode##im", &modeIdx, modeLabels, 2)) {
                        m->emissionMode = (HomogeneousMedium::EmissionMode)modeIdx;
                        changed = true;
                    }
                    if (m->emissionMode == HomogeneousMedium::EmissionMode_Constant) {
                        changed |= ImGui::DragFloat3("emission##im", se, 0.05f, 0.0f, 100.0f, "%.3f");
                    } else {
                        // Cone params. With coneFollowObject ON (the default
                        // for newly-created media in the viewer), coneOrigin
                        // and coneAxis are interpreted in the SceneObject's
                        // *local* space — moving the bounding mesh moves the
                        // flame. Toggle it off if you authored world-space
                        // params in JSON and want to keep that behaviour.
                        bool coneChanged = false;
                        coneChanged |= ImGui::Checkbox("followObject##im", &m->coneFollowObject);
                        ImGui::SameLine();
                        ImGui::TextDisabled(m->coneFollowObject ? "(object-local)" : "(world-space)");
                        float coAxis[3]   = { m->coneAxis.x,   m->coneAxis.y,   m->coneAxis.z };
                        float coOrigin[3] = { m->coneOrigin.x, m->coneOrigin.y, m->coneOrigin.z };
                        float coIn[3]     = { m->coneInner.r,  m->coneInner.g,  m->coneInner.b };
                        float coOut[3]    = { m->coneOuter.r,  m->coneOuter.g,  m->coneOuter.b };
                        coneChanged |= ImGui::DragFloat3("coneOrigin##im",   coOrigin, 0.05f, -1000.0f, 1000.0f, "%.3f");
                        coneChanged |= ImGui::DragFloat3("coneAxis##im",     coAxis,   0.05f, -1.0f, 1.0f, "%.3f");
                        coneChanged |= ImGui::SliderFloat("coneLength##im", &m->coneLength,   0.05f, 20.0f, "%.3f");
                        coneChanged |= ImGui::SliderFloat("coneRadius##im", &m->coneRadius,   0.01f, 5.0f,  "%.3f");
                        coneChanged |= ImGui::ColorEdit3 ("coneInner##im",   coIn);
                        coneChanged |= ImGui::ColorEdit3 ("coneOuter##im",   coOut);
                        coneChanged |= ImGui::DragFloat  ("coneIntensity##im",     &m->coneIntensity,     1.0f, 0.0f, 10000.0f, "%.1f");
                        coneChanged |= ImGui::SliderFloat("conePeakAxial##im",     &m->conePeakAxial,     0.0f, 1.0f, "%.3f");
                        coneChanged |= ImGui::SliderFloat("conePeakSharpness##im", &m->conePeakSharpness, 0.5f, 20.0f, "%.2f");
                        coneChanged |= ImGui::SliderInt  ("emissionSamples##im",   &m->coneEmissionSamples, 1, 32);
                        if (coneChanged) {
                            m->coneAxis   = vec3(coAxis[0],   coAxis[1],   coAxis[2]);
                            m->coneOrigin = vec3(coOrigin[0], coOrigin[1], coOrigin[2]);
                            m->coneInner  = color3(coIn[0],   coIn[1],   coIn[2]);
                            m->coneOuter  = color3(coOut[0],  coOut[1],  coOut[2]);
                            changed = true;
                        }
                    }

                    // Phase 3: density field. fBm noise modulates σa/σs/σe
                    // at each ray point — turns uniform fog into wispy
                    // clouds, smooth flames into turbulent ones. Authoring
                    // tip: noiseBias=-0.2 carves empty pockets ("wisps");
                    // noiseFrequency sets the world-space scale of detail.
                    const char* dfLabels[] = { "None", "fBm noise" };
                    int dfIdx = (int)m->densityField;
                    if (ImGui::Combo("densityField##im", &dfIdx, dfLabels, 2)) {
                        m->densityField = (HomogeneousMedium::DensityFieldMode)dfIdx;
                        changed = true;
                    }
                    if (m->densityField == HomogeneousMedium::DensityField_FBmNoise) {
                        float noff[3] = { m->noiseOffset.x, m->noiseOffset.y, m->noiseOffset.z };
                        bool nChanged = false;
                        nChanged |= ImGui::SliderFloat("noiseFrequency##im",  &m->noiseFrequency,  0.05f, 16.0f, "%.3f");
                        nChanged |= ImGui::SliderInt  ("noiseOctaves##im",    &m->noiseOctaves,    1, 6);
                        nChanged |= ImGui::SliderFloat("noiseGain##im",       &m->noiseGain,       0.0f, 1.0f, "%.3f");
                        nChanged |= ImGui::SliderFloat("noiseLacunarity##im", &m->noiseLacunarity, 1.0f, 4.0f, "%.3f");
                        nChanged |= ImGui::SliderFloat("noiseAmplitude##im",  &m->noiseAmplitude,  0.0f, 4.0f, "%.3f");
                        nChanged |= ImGui::SliderFloat("noiseBias##im",       &m->noiseBias,      -1.0f, 1.0f, "%.3f");
                        nChanged |= ImGui::DragFloat3 ("noiseOffset##im",     noff, 0.05f, -1000.0f, 1000.0f, "%.3f");
                        if (nChanged) {
                            m->noiseOffset = vec3(noff[0], noff[1], noff[2]);
                            changed = true;
                        }
                    }

                    if (changed) {
                        m->sigma_a = color3(sa[0], sa[1], sa[2]);
                        m->sigma_s = color3(ss[0], ss[1], ss[2]);
                        m->sigma_e = color3(se[0], se[1], se[2]);
                        m->prepare();
                        sceneDirty = true;
                    }
                }
            }
        }
        ImGui::End();

        // Scene edits always need a full re-trace (BVH bounds, transforms,
        // materials all feed the primary ray). Route through the shared
        // pendingDirty machinery; uiParams is unchanged, so
        // onlyPostProcessChanged() returns false and the kick runs as Full.
        if (sceneDirty) {
            pendingDirty = true;
        }

        // Auto-kick: the single site for slider-driven renders. Placed after
        // every panel so it sees widget activity from any of them (control
        // panel, Outline, Property) without a one-frame lag.
        //
        // Live preview: while ANY ImGui item is held (slider drag, ColorEdit
        // picker, DragFloat3 scrub), override samples to 1 so feedback stays
        // interactive. On release, fall through to one more kick with the
        // user's real sample count — that's the "final" render that also
        // gets persisted to the sidecar. The `lastKickWasPreview` gate keeps
        // non-slider widget releases (button taps etc.) from triggering a
        // redundant re-render when their kick already ran at full quality.
        const bool anyActive     = ImGui::IsAnyItemActive();
        static bool prevActive   = false;
        static bool lastKickWasPreview = false;
        const bool justReleased  = prevActive && !anyActive;
        prevActive = anyActive;

        const bool wantPreview = anyActive && uiParams.samples > 1;

        if (!isRendering && (pendingDirty || (justReleased && lastKickWasPreview))) {
            // Kind decision uses uiParams — the user's real intent. Feeding
            // the preview snapshot here would let the forced samples=1 pose
            // as "samples changed" on every drag frame, routing a bloom edit
            // to a Full trace instead of the cheap PostOnly rebloom.
            const JobKind kind =
                onlyPostProcessChanged(lastKickedParams, uiParams)
                    ? JobKind::PostOnly : JobKind::Full;

            // Only Full renders care about samples; PostOnly replays bloom
            // over the cached HDR image, so leaving samples alone there also
            // avoids clobbering renderer.settings.samples in the rare cache-
            // miss fallback (where PostOnly degrades into a full render()).
            ViewerParams snapshot = uiParams;
            const bool applyOverride = (kind == JobKind::Full) && wantPreview;
            if (applyOverride) snapshot.samples = 1;

            lastKickedParams = uiParams;
            enqueueWorker(snapshot, kind);
            lastKickWasPreview = applyOverride;
            // Only persist when we're at rest — preview snapshots with
            // forced samples=1 would otherwise overwrite the user-set value.
            if (!anyActive) persistSidecar();
            pendingDirty = false;
        }

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
