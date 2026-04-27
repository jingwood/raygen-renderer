///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — shared types used across panels (params, job kind, diff).
//
//  Pulled out of main.cpp so per-panel files (MainPanel, PropertyPanel, ...)
//  can reference the same ViewerParams / JobKind without including each other.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_types_h__
#define __viewer_types_h__

namespace raygen {
namespace viewer {

// Tunable parameters driven by the UI. All plain old data so the worker
// thread can snapshot them under a mutex without any lifetime concerns.
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

// What a render-worker job is supposed to do. PostOnly skips raytracing and
// re-runs bloom against the cached HDR snapshot — cheap, used when only bloom
// sliders moved. Full retraces from scratch.
enum class JobKind { Full, PostOnly };

// Returns true iff the only thing that changed between `a` and `b` is a
// post-process parameter. Caller uses this to route the next render to a
// JobKind::PostOnly instead of a Full retrace.
bool onlyPostProcessChanged(const ViewerParams& a, const ViewerParams& b);

}  // namespace viewer
}  // namespace raygen

#endif
