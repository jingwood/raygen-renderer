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
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>

#ifdef _WIN32
  #include <direct.h>
  #include <windows.h>
#else
  #include <sys/stat.h>
  #include <sys/types.h>
#endif

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
#include "raygen/scenewriter.h"

#include "Dialog.h"
#include "FilePanel.h"
#include "MainPanel.h"
#include "MediumEditor.h"
#include "OutlinePanel.h"
#include "PropertyPanel.h"
#include "ViewerTypes.h"
#include "ugm/image.h"
#include "ugm/imgcodec.h"
#include "ucm/file.h"
#include "ucm/jsonreader.h"
#include "ucm/jsonwriter.h"
#include "ucm/jstypes.h"
#include "ucm/string.h"

using namespace raygen;
using namespace ugm;
using raygen::viewer::ViewerParams;
using raygen::viewer::JobKind;
using raygen::viewer::onlyPostProcessChanged;

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

// Sidecar schema version. v1 is the first nested layout; v0 (no version key)
// is the original flat layout we still read for back-compat.
//
// Layout (v1) — mirrors scene.json's hierarchy so a future "merged dump" can
// overlay the sidecar onto the scene tree directly:
//
//   {
//     "schemaVersion": 1,
//     "quality":     { samples, threads, denoise, denoiseIntensity,
//                      adaptiveSampling, adaptiveBaseSamples, adaptiveThreshold },
//     "mainCamera":  { location, angle, fieldOfView, depthOfField, aperture,
//                      apertureBlades, apertureRotation, exposure },
//     "envmap":      { intensity, rotation },
//     "medium":      { enabled, sigmaA, sigmaS, emission, g, density },
//     "postProcess": { enabled, bloom: { threshold, strength, curve, radius } },
//     "output":      { width, height },
//
//     // Future expansion. Children must live under an explicit "children"
//     // key so transform/material fields can't collide with object names:
//     // "objects": {
//     //   "<name>": {
//     //     "transform": { location, angle, scale },
//     //     "visible": true, "renderable": true,
//     //     "material": { ... },
//     //     "children": { "<childName>": { ... } }
//     //   }
//     // }
//   }
static const int VIEWER_SCHEMA_VERSION = 1;

// Read a JSON array of 3 numbers into `v`. Silently leaves `v` untouched if
// the key is missing or the array is the wrong shape — defaults stay in place.
static void readVec3Into(const ucm::JSObject* obj, const char* key, float v[3]) {
    if (!obj) return;
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

// Legacy flat reader for v0 sidecars (no schemaVersion field). Pulls the same
// keys the original layout used; can be deleted once existing sidecars in the
// wild are confirmed migrated.
static void loadViewerConfigV0(const ucm::JSObject* obj, ViewerParams& params,
                               int& outputWidth, int& outputHeight) {
    obj->tryGetNumberProperty("samples",          &params.samples);
    obj->tryGetNumberProperty("threads",          &params.threads);
    if (obj->hasProperty("denoise"))
        params.denoise = obj->isBooleanPropertyTrue("denoise");
    obj->tryGetNumberProperty("denoiseIntensity", &params.denoiseIntensity);
    if (obj->hasProperty("adaptiveSampling"))
        params.adaptiveSampling = obj->isBooleanPropertyTrue("adaptiveSampling");
    obj->tryGetNumberProperty("adaptiveBaseSamples", &params.adaptiveBaseSamples);
    obj->tryGetNumberProperty("adaptiveThreshold",   &params.adaptiveThreshold);
    readVec3Into(obj, "location", params.camLocation);
    readVec3Into(obj, "angle",    params.camAngle);
    obj->tryGetNumberProperty("fieldOfView",      &params.fieldOfView);
    obj->tryGetNumberProperty("depthOfField",     &params.depthOfField);
    obj->tryGetNumberProperty("aperture",         &params.aperture);
    obj->tryGetNumberProperty("apertureBlades",   &params.apertureBlades);
    obj->tryGetNumberProperty("apertureRotation", &params.apertureRotation);
    obj->tryGetNumberProperty("exposure",         &params.exposure);
    obj->tryGetNumberProperty("envIntensity",     &params.envIntensity);
    obj->tryGetNumberProperty("envRotation",      &params.envRotation);
    if (obj->hasProperty("mediumEnabled"))
        params.mediumEnabled = obj->isBooleanPropertyTrue("mediumEnabled");
    readVec3Into(obj, "mediumSigmaA",   params.mediumSigmaA);
    readVec3Into(obj, "mediumSigmaS",   params.mediumSigmaS);
    readVec3Into(obj, "mediumEmission", params.mediumEmission);
    obj->tryGetNumberProperty("mediumG",       &params.mediumG);
    obj->tryGetNumberProperty("mediumDensity", &params.mediumDensity);
    if (obj->hasProperty("postProcess"))
        params.postProcess = obj->isBooleanPropertyTrue("postProcess");
    obj->tryGetNumberProperty("bloomThreshold",   &params.bloomThreshold);
    obj->tryGetNumberProperty("bloomStrength",    &params.bloomStrength);
    obj->tryGetNumberProperty("bloomCurve",       &params.bloomCurve);
    obj->tryGetNumberProperty("bloomRadius",      &params.bloomRadius);
    obj->tryGetNumberProperty("outputWidth",      &outputWidth);
    obj->tryGetNumberProperty("outputHeight",     &outputHeight);
}

static void loadViewerConfigV1(const ucm::JSObject* root, ViewerParams& params,
                               int& outputWidth, int& outputHeight) {
    if (const ucm::JSObject* q = root->getObjectProperty("quality")) {
        q->tryGetNumberProperty("samples",          &params.samples);
        q->tryGetNumberProperty("threads",          &params.threads);
        if (q->hasProperty("denoise"))
            params.denoise = q->isBooleanPropertyTrue("denoise");
        q->tryGetNumberProperty("denoiseIntensity", &params.denoiseIntensity);
        if (q->hasProperty("adaptiveSampling"))
            params.adaptiveSampling = q->isBooleanPropertyTrue("adaptiveSampling");
        q->tryGetNumberProperty("adaptiveBaseSamples", &params.adaptiveBaseSamples);
        q->tryGetNumberProperty("adaptiveThreshold",   &params.adaptiveThreshold);
    }
    if (const ucm::JSObject* c = root->getObjectProperty("mainCamera")) {
        readVec3Into(c, "location", params.camLocation);
        readVec3Into(c, "angle",    params.camAngle);
        c->tryGetNumberProperty("fieldOfView",      &params.fieldOfView);
        c->tryGetNumberProperty("depthOfField",     &params.depthOfField);
        c->tryGetNumberProperty("aperture",         &params.aperture);
        c->tryGetNumberProperty("apertureBlades",   &params.apertureBlades);
        c->tryGetNumberProperty("apertureRotation", &params.apertureRotation);
        c->tryGetNumberProperty("exposure",         &params.exposure);
    }
    if (const ucm::JSObject* e = root->getObjectProperty("envmap")) {
        e->tryGetNumberProperty("intensity", &params.envIntensity);
        e->tryGetNumberProperty("rotation",  &params.envRotation);
    }
    if (const ucm::JSObject* m = root->getObjectProperty("medium")) {
        if (m->hasProperty("enabled"))
            params.mediumEnabled = m->isBooleanPropertyTrue("enabled");
        readVec3Into(m, "sigmaA",   params.mediumSigmaA);
        readVec3Into(m, "sigmaS",   params.mediumSigmaS);
        readVec3Into(m, "emission", params.mediumEmission);
        m->tryGetNumberProperty("g",       &params.mediumG);
        m->tryGetNumberProperty("density", &params.mediumDensity);
    }
    if (const ucm::JSObject* p = root->getObjectProperty("postProcess")) {
        if (p->hasProperty("enabled"))
            params.postProcess = p->isBooleanPropertyTrue("enabled");
        if (const ucm::JSObject* b = p->getObjectProperty("bloom")) {
            b->tryGetNumberProperty("threshold", &params.bloomThreshold);
            b->tryGetNumberProperty("strength",  &params.bloomStrength);
            b->tryGetNumberProperty("curve",     &params.bloomCurve);
            b->tryGetNumberProperty("radius",    &params.bloomRadius);
        }
    }
    if (const ucm::JSObject* o = root->getObjectProperty("output")) {
        o->tryGetNumberProperty("width",  &outputWidth);
        o->tryGetNumberProperty("height", &outputHeight);
    }
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

    int version = 0;
    obj->tryGetNumberProperty("schemaVersion", &version);
    if (version >= 1) {
        loadViewerConfigV1(obj, params, outputWidth, outputHeight);
    } else {
        // No version key → legacy flat layout. The next saveViewerConfig() will
        // rewrite the file in v1 form.
        loadViewerConfigV0(obj, params, outputWidth, outputHeight);
    }

    delete obj;
    return true;
}

static void saveViewerConfig(const char* path, const ViewerParams& params,
                             int outputWidth, int outputHeight) {
    ucm::JSONWriter w;
    w.beginObject();
    w.writeProperty("schemaVersion", VIEWER_SCHEMA_VERSION);

    w.beginObjectWithKey("quality");
        w.writeProperty("samples",             (int)params.samples);
        w.writeProperty("threads",             (int)params.threads);
        w.writeProperty("denoise",             params.denoise);
        w.writeProperty("denoiseIntensity",    (double)params.denoiseIntensity);
        w.writeProperty("adaptiveSampling",    params.adaptiveSampling);
        w.writeProperty("adaptiveBaseSamples", (int)params.adaptiveBaseSamples);
        w.writeProperty("adaptiveThreshold",   (double)params.adaptiveThreshold);
    w.endObject();

    w.beginObjectWithKey("mainCamera");
        writeVec3(w,    "location",         params.camLocation);
        writeVec3(w,    "angle",            params.camAngle);
        w.writeProperty("fieldOfView",      (double)params.fieldOfView);
        w.writeProperty("depthOfField",     (double)params.depthOfField);
        w.writeProperty("aperture",         (double)params.aperture);
        w.writeProperty("apertureBlades",   (int)params.apertureBlades);
        w.writeProperty("apertureRotation", (double)params.apertureRotation);
        w.writeProperty("exposure",         (double)params.exposure);
    w.endObject();

    w.beginObjectWithKey("envmap");
        w.writeProperty("intensity", (double)params.envIntensity);
        w.writeProperty("rotation",  (double)params.envRotation);
    w.endObject();

    w.beginObjectWithKey("medium");
        w.writeProperty("enabled",  params.mediumEnabled);
        writeVec3(w,    "sigmaA",   params.mediumSigmaA);
        writeVec3(w,    "sigmaS",   params.mediumSigmaS);
        writeVec3(w,    "emission", params.mediumEmission);
        w.writeProperty("g",        (double)params.mediumG);
        w.writeProperty("density",  (double)params.mediumDensity);
    w.endObject();

    w.beginObjectWithKey("postProcess");
        w.writeProperty("enabled", params.postProcess);
        w.beginObjectWithKey("bloom");
            w.writeProperty("threshold", (double)params.bloomThreshold);
            w.writeProperty("strength",  (double)params.bloomStrength);
            w.writeProperty("curve",     (double)params.bloomCurve);
            w.writeProperty("radius",    (double)params.bloomRadius);
        w.endObject();
    w.endObject();

    w.beginObjectWithKey("output");
        w.writeProperty("width",  outputWidth);
        w.writeProperty("height", outputHeight);
    w.endObject();

    w.endObject();
    ucm::File::writeTextFile(path, w.getString().getBuffer());
}

///////////////////////////////////////////////////////////////////////////////
// Per-user UI state: window geometry + ImGui panel layout.
//
// Lives in a global config directory (not next to the scene), since the user's
// preferred window position and panel layout doesn't change with the scene.
//   POSIX:   $HOME/.raygen-viewer/
//   Windows: %APPDATA%/raygen-viewer/
//
// Two files inside that dir:
//   imgui.ini   — ImGui handles this automatically once io.IniFilename is set.
//                 Stores per-panel docking, position, size, collapsed state.
//   window.json — our own GLFW window x/y/width/height. Schema versioned to
//                 stay aligned with how the scene sidecar evolves.
///////////////////////////////////////////////////////////////////////////////

static const int VIEWER_WINDOW_SCHEMA_VERSION = 1;

static bool makeDirIfMissing(const char* path) {
#ifdef _WIN32
    int rc = _mkdir(path);
    return rc == 0 || errno == EEXIST;
#else
    int rc = ::mkdir(path, 0755);
    return rc == 0 || errno == EEXIST;
#endif
}

// Fill `out` with "<configDir>/" (with trailing separator). Creates the
// directory if missing. Returns false if no usable home/appdata env var was
// found — callers fall back to in-CWD defaults so the viewer still runs.
static bool ensureViewerConfigDir(char* out, size_t outCap) {
#ifdef _WIN32
    const char* base = getenv("APPDATA");
    const char sep = '\\';
#else
    const char* base = getenv("HOME");
    const char sep = '/';
#endif
    if (!base || !*base) { out[0] = '\0'; return false; }

#ifdef _WIN32
    int n = std::snprintf(out, outCap, "%s%craygen-viewer%c", base, sep, sep);
#else
    int n = std::snprintf(out, outCap, "%s%c.raygen-viewer%c", base, sep, sep);
#endif
    if (n <= 0 || (size_t)n >= outCap) { out[0] = '\0'; return false; }

    // mkdir wants the path without trailing separator on POSIX; trim, mkdir,
    // restore. Cheap, avoids platform-specific helpers.
    out[n - 1] = '\0';
    bool ok = makeDirIfMissing(out);
    out[n - 1] = sep;
    return ok;
}

// Compose "<dir><file>" into `out`. Caller-provided buffer; truncates safely.
static void joinConfigPath(char* out, size_t outCap, const char* dir, const char* file) {
    std::snprintf(out, outCap, "%s%s", dir, file);
}

// Load saved window geometry. Returns true if file existed and parsed; on
// false the caller uses hard-coded defaults. Individual fields default to
// their input value if missing, so partial/older files are tolerated.
static bool loadWindowState(const char* path, int& x, int& y, int& w, int& h) {
    ucm::string pathStr(path);
    ucm::File f(pathStr);
    if (!f.isExist()) return false;
    ucm::string json;
    ucm::File::readTextFile(path, json);
    if (json.length() == 0) return false;

    ucm::JSONReader reader(json);
    ucm::JSObject* root = reader.readObject();
    if (!root) return false;

    if (const ucm::JSObject* win = root->getObjectProperty("window")) {
        win->tryGetNumberProperty("x",      &x);
        win->tryGetNumberProperty("y",      &y);
        win->tryGetNumberProperty("width",  &w);
        win->tryGetNumberProperty("height", &h);
    }
    delete root;
    return true;
}

static void saveWindowState(const char* path, int x, int y, int w, int h) {
    ucm::JSONWriter wr;
    wr.beginObject();
    wr.writeProperty("schemaVersion", VIEWER_WINDOW_SCHEMA_VERSION);
    wr.beginObjectWithKey("window");
        wr.writeProperty("x",      x);
        wr.writeProperty("y",      y);
        wr.writeProperty("width",  w);
        wr.writeProperty("height", h);
    wr.endObject();
    wr.endObject();
    ucm::File::writeTextFile(path, wr.getString().getBuffer());
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
    s.enableAdaptiveSampling    = p.adaptiveSampling;
    s.adaptiveBaseSamples       = p.adaptiveBaseSamples;
    s.adaptiveThreshold         = p.adaptiveThreshold;
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
    // Scene argument is optional now that the File panel has a Load Scene
    // dialog: when omitted we boot with an empty Scene and the user picks
    // one at runtime. scenePath stays a 1024-byte mutable buffer either way
    // so the dialog can write into it without reallocating.
    char scenePath[1024] = {0};
    if (argc >= 2 && argv[1] != nullptr) {
        const char* a = argv[1];
        size_t n = std::strlen(a);
        if (n >= sizeof(scenePath)) n = sizeof(scenePath) - 1;
        std::memcpy(scenePath, a, n);
        scenePath[n] = '\0';
    }

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    // Per-user UI state directory. If we can't compute one (no HOME/APPDATA)
    // the viewer still runs — imgui.ini falls back to its default location and
    // window geometry uses the hard-coded fallback below.
    static char viewerConfigDir[1024] = {0};
    static char imguiIniPath[1024]    = {0};
    char windowStatePath[1024]        = {0};
    const bool haveConfigDir = ensureViewerConfigDir(viewerConfigDir, sizeof(viewerConfigDir));
    if (haveConfigDir) {
        joinConfigPath(imguiIniPath,    sizeof(imguiIniPath),    viewerConfigDir, "imgui.ini");
        joinConfigPath(windowStatePath, sizeof(windowStatePath), viewerConfigDir, "window.json");
    }

    // Window geometry: load saved values; fall back to a sensible default size
    // and let the WM pick the position (-1 sentinel = "don't restore").
    int winX = -1, winY = -1, winW = 1500, winH = 950;
    if (haveConfigDir) loadWindowState(windowStatePath, winX, winY, winW, winH);
    if (winW < 320) winW = 1500;
    if (winH < 200) winH = 950;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(winW, winH, "raygen viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    if (winX != -1 && winY != -1) {
        // GLFW silently no-ops if the position is invalid for the current
        // monitor configuration, so we don't need to clamp ourselves.
        glfwSetWindowPos(window, winX, winY);
    }
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
    // Point ImGui at our per-user imgui.ini so panel positions, sizes, and
    // collapsed state survive across runs. ImGui doesn't copy this string, so
    // imguiIniPath must outlive the ImGui context — it's a static buffer.
    // When the config dir is unavailable we leave IniFilename at its default
    // ("imgui.ini" in CWD) rather than disabling persistence outright.
    if (haveConfigDir && imguiIniPath[0]) {
        io.IniFilename = imguiIniPath;
    }
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
    // Empty scene when no path was given on the CLI: the user will Load one.
    auto scene = std::make_unique<Scene>();
    if (scenePath[0] != '\0') {
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
        p.adaptiveSampling     = renderer.settings.enableAdaptiveSampling;
        p.adaptiveBaseSamples  = renderer.settings.adaptiveBaseSamples;
        p.adaptiveThreshold    = renderer.settings.adaptiveThreshold;
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
    // something sensible. With no scene yet, fall back to a generic name in
    // the CWD; Load Scene will rewrite it once the user picks a file.
    char outputPath[512] = {0};
    if (scenePath[0] != '\0') {
        const char* dot = strrchr(scenePath, '.');
        size_t baseLen = dot ? (size_t)(dot - scenePath) : strlen(scenePath);
        if (baseLen > sizeof(outputPath) - 16) baseLen = sizeof(outputPath) - 16;
        memcpy(outputPath, scenePath, baseLen);
        memcpy(outputPath + baseLen, "-out.jpg", 9);
    } else {
        std::memcpy(outputPath, "untitled-out.jpg", 17);
    }

    // Sidecar "<scene>.viewer.json": auto-loaded here (scene-seeded values
    // act as fallbacks for missing keys) and auto-saved on every render kick
    // further down. Lets us persist slider state across sessions without
    // touching scene.json (which preserves JSONC comments and author intent).
    // Empty when no scene yet — persistSidecar/Load skip both ends.
    char sidecarPath[512] = {0};
    if (scenePath[0] != '\0') {
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
    // mutex so a slow disk can't stall the handoff. With no scene loaded,
    // sidecarPath is empty and we'd be writing to "viewer.json" in the CWD —
    // skip until a scene actually backs the slider state.
    auto persistSidecar = [&]() {
        if (sidecarPath[0] == '\0') return;
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
    // Skip the initial render when no scene was given — the user kicks one
    // by choosing Load Scene. Avoids tracing an empty BVH on startup just
    // to produce a black frame.
    if (scenePath[0] != '\0') kickFinal();

    GLuint renderTex = 0;
    int   texW = 0, texH = 0;
    double lastRenderSec = 0.0;
    // True only while the worker is busy — initialized from whether we
    // actually kicked the initial render above.
    bool  isRendering = (scenePath[0] != '\0');
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

        // --- Control panel (lives in MainPanel.cpp) ---
        viewer::MainPanelCtx mpCtx;
        mpCtx.params           = &uiParams;
        mpCtx.lastKickedParams = &lastKickedParams;
        mpCtx.pendingDirty     = &pendingDirty;
        mpCtx.mainCamera       = scene->mainCamera;
        mpCtx.renderer         = &renderer;
        mpCtx.scenePath        = scenePath;
        mpCtx.fps              = io.Framerate;
        mpCtx.lastRenderSec    = (float)lastRenderSec;
        mpCtx.isRendering      = isRendering;
        mpCtx.currentJobKind   = currentJobKind;
        mpCtx.previewProgress  = previewProgress;
        viewer::drawMainPanel(mpCtx, kickFinal);

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

        // --- File window (lives in FilePanel.cpp) ---
        // Reload uses the *current* scenePath. Load updates scenePath (and
        // sidecar / default output path) before running the same flow.
        // Both touch the Scene unique_ptr, so the panel disables them while
        // the worker is rendering.
        auto reloadCurrentScene = [&]() {
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

            // Re-seed from scene then overlay the sidecar, so reload behaves
            // symmetrically with initial load: user tweaks win, scene
            // defaults fill in the rest.
            seedUiParamsFromScene(uiParams);
            int outW = renderer.settings.resolutionWidth;
            int outH = renderer.settings.resolutionHeight;
            if (loadViewerConfig(sidecarPath, uiParams, outW, outH)) {
                renderer.settings.resolutionWidth  = outW;
                renderer.settings.resolutionHeight = outH;
                renderer.setRenderSize(outW, outH);
                outputWidth  = outW;
                outputHeight = outH;
            }

            lastKickedParams = uiParams;
            kickFinal(JobKind::Full);
        };
        auto loadNewScene = [&](const char* newPath) {
            // Persist current scene's sidecar before we lose track of its
            // path. sidecarPath still points at the *outgoing* scene here, so
            // any edits made since the last explicit save aren't dropped on
            // the floor. No-op when sidecarPath is empty (first-time load).
            persistSidecar();

            // Replace scenePath, recompute the sidecar location, and reset
            // the default output path to "<basename>-out.jpg" — user-edited
            // output paths from the previous scene shouldn't carry over.
            size_t n = std::strlen(newPath);
            if (n >= sizeof(scenePath)) n = sizeof(scenePath) - 1;
            std::memcpy(scenePath, newPath, n);
            scenePath[n] = '\0';

            computeSidecarPath(scenePath, sidecarPath, sizeof(sidecarPath));

            const char* dot = std::strrchr(scenePath, '.');
            size_t baseLen = dot ? (size_t)(dot - scenePath) : std::strlen(scenePath);
            if (baseLen > sizeof(outputPath) - 16) baseLen = sizeof(outputPath) - 16;
            std::memcpy(outputPath, scenePath, baseLen);
            std::memcpy(outputPath + baseLen, "-out.jpg", 9);

            reloadCurrentScene();
        };

        // Save bundle: package the *current* in-memory Scene (with all the
        // viewer-side edits to transforms, materials, mediums) into a single
        // .toba archive. The worker is gated off in the panel via isRendering
        // so this walk doesn't race with mid-render mesh / material reads.
        auto saveBundle = [&]() {
            // Seed default filename from the loaded scene path: drop the
            // existing extension, append `.toba`. With no scene yet we offer
            // "untitled.toba" — user can re-aim with the dialog.
            char defaultName[256] = {0};
            if (scenePath[0] != '\0') {
                const char* slashU = std::strrchr(scenePath, '/');
                const char* slashW = std::strrchr(scenePath, '\\');
                const char* slash  = slashU > slashW ? slashU : slashW;
                const char* base   = slash ? slash + 1 : scenePath;
                size_t baseLen = std::strlen(base);
                const char* dot = std::strrchr(base, '.');
                if (dot) baseLen = (size_t)(dot - base);
                if (baseLen > sizeof(defaultName) - 6) baseLen = sizeof(defaultName) - 6;
                std::memcpy(defaultName, base, baseLen);
                std::memcpy(defaultName + baseLen, ".toba", 6);
            } else {
                std::memcpy(defaultName, "untitled.toba", 14);
            }

            char initDir[512] = {0};
            if (scenePath[0] != '\0') {
                const char* slashU = std::strrchr(scenePath, '/');
                const char* slashW = std::strrchr(scenePath, '\\');
                const char* slash  = slashU > slashW ? slashU : slashW;
                if (slash != nullptr) {
                    size_t n = (size_t)(slash - scenePath);
                    if (n >= sizeof(initDir)) n = sizeof(initDir) - 1;
                    std::memcpy(initDir, scenePath, n);
                    initDir[n] = '\0';
                }
            }

            char picked[1024] = {0};
            if (!viewer::saveBundleFileDialog(picked, sizeof(picked),
                                              defaultName,
                                              initDir[0] ? initDir : nullptr)) {
                return;
            }

            // Use the latest tonemapped frame as the bundle thumbnail. Skip
            // when there's no rendered frame yet — SceneBundleSaver handles a
            // null thumbnail by simply not creating chunk uid=2.
            const ugm::Image* thumb = nullptr;
            if (renderTex != 0) {
                thumb = &renderer.getRenderResult();
            }

            try {
                raygen::SceneBundleSaver::save(*scene, ucm::string(picked), thumb);
                fprintf(stdout, "saved bundle: %s\n", picked);
            } catch (const std::exception& e) {
                fprintf(stderr, "save bundle failed: %s\n", e.what());
            } catch (...) {
                fprintf(stderr, "save bundle failed (unknown error)\n");
            }
        };

        viewer::FilePanelCtx fpCtx;
        fpCtx.scenePath      = scenePath;
        fpCtx.outputPath     = outputPath;
        fpCtx.outputPathCap  = sizeof(outputPath);
        fpCtx.isRendering    = isRendering;
        fpCtx.hasRender      = (renderTex != 0);
        fpCtx.renderer       = &renderer;
        fpCtx.onReloadScene  = reloadCurrentScene;
        fpCtx.onLoadScene    = loadNewScene;
        fpCtx.onSaveViewer   = persistSidecar;
        fpCtx.onSaveBundle   = saveBundle;
        viewer::drawFilePanel(fpCtx);

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
        // --- Outline + Property windows (live in OutlinePanel.cpp /
        // PropertyPanel.cpp). Outline owns the selection cursor; Property
        // reads it.
        bool sceneDirty = false;
        sceneDirty |= viewer::drawOutlinePanel(*scene, selectedObj);
        sceneDirty |= viewer::drawPropertyPanel(selectedObj);

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
            // Slider-driven kicks no longer auto-persist — too noisy and
            // overwrites the file mid-tweak. Sidecar saves now happen on
            // explicit actions (Apply / Re-render / Reload / Load scene /
            // Save viewer button) via persistSidecar() in those paths.
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

    // Capture window geometry before tearing down GLFW. ImGui auto-saves
    // imgui.ini during DestroyContext using the IniFilename we configured.
    if (haveConfigDir && windowStatePath[0]) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetWindowPos(window, &x, &y);
        glfwGetWindowSize(window, &w, &h);
        if (w > 0 && h > 0) saveWindowState(windowStatePath, x, y, w, h);
    }

    if (renderTex) glDeleteTextures(1, &renderTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
