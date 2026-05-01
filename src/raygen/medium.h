///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __raygen_medium_h__
#define __raygen_medium_h__

// ugm.h does the global `using namespace ugm` that makes the unqualified
// color3/vec3 typedefs visible inside `namespace raygen` — matches how
// material.h / scene.h pick them up via texture.h.
#include "ugm/ugm.h"

#include <vector>

namespace raygen {

// Phase 1 participating media: homogeneous absorption + isotropic-scattering
// volume with optional uniform emission. Per-channel σ values let absorption
// be wavelength-dependent (Beer-Lambert color tint for water/glass/blood),
// and a Henyey-Greenstein phase function captures the forward-/back-scatter
// anisotropy of clouds, smoke, water particulate, etc.
//
// Phase 2 layers a procedural emission profile on top: when `emissionMode` is
// Cone, σe is no longer the constant `sigma_e` but a position-dependent field
// shaped like a jet-engine afterburner — hot blue-white core fading to an
// orange envelope along an axis. The σa/σs/g fields keep their Phase 1 role,
// so a flame can also absorb/scatter behind itself if you want soot.
//
// All σ values are in inverse world-distance units (1/m if scene is metres).
// `density` multiplies σa/σs/σe so artists can dial overall thickness without
// re-balancing the colour. The cone profile multiplies by `density` too — so
// thinning a flame keeps its color but dims it.
class HomogeneousMedium {
public:
    // Constant: σe is the uniform `sigma_e` field (analytical integral).
    // Cone:     σe(p) is procedural along an axis, sampled along the ray.
    // Path:     σe(p) is a swept-tube along a poly-line. Each sample finds
    //           the closest segment, evaluates a radial falloff inside the
    //           tube, and blends pathInner→pathOuter by the per-control-
    //           point axial parameter. Lets vapor follow a curved streamline
    //           rather than a single straight axis — used for wing-surface
    //           vortex condensation that hugs the airframe and lifts off
    //           toward the trailing edge.
    enum EmissionMode {
        EmissionMode_Constant = 0,
        EmissionMode_Cone     = 1,
        EmissionMode_Path     = 2,
    };

    // Per-control-point sample for Path emission. `p` is a 3D anchor in
    // world or object-local space (see `pathFollowObject`); `radius` is the
    // tube radius at this point (lerped between adjacent points along the
    // segment); `t` is the axial parameter ∈ [0,1] used to blend
    // pathInner→pathOuter (lerped likewise).
    struct PathSample {
        vec3  p      = vec3::zero;
        float radius = 0.1f;
        float t      = 0.0f;
    };

    // None: σ values are spatially uniform — Phase 1 / Phase 2 behaviour.
    // FBmNoise: σ values are multiplied by a fractal-Brownian-motion field
    //           sampled at each ray point. Turns uniform fog into wispy
    //           clouds, smooth flames into turbulent ones. Free-flight then
    //           uses delta tracking (σt_max bound) instead of the
    //           closed-form exponential.
    enum DensityFieldMode {
        DensityField_None     = 0,
        DensityField_FBmNoise = 1,
    };

    // Per-channel coefficients in the radiative transfer equation.
    color3 sigma_a = color3::zero;   // absorption
    color3 sigma_s = color3::zero;   // scattering
    color3 sigma_e = color3::zero;   // emission (Constant mode only — fire, plasma)

    // Henyey-Greenstein anisotropy [-1, 1]. 0 = isotropic. >0 = forward-
    // peaked (clouds, foggy haze), <0 = back-scattering. Values past ~±0.95
    // start aliasing badly under finite-sample MIS.
    float g = 0.0f;

    // Multiplier on the σ trio. Authoring convenience: keep the colour ratio
    // fixed, scale density to thicken/thin the volume.
    float density = 1.0f;

    // --- Phase 2: procedural emission (jet flame / afterburner shape) ------
    EmissionMode emissionMode = EmissionMode_Constant;
    // World-space anchor + axis for the cone. Negative axial fraction (behind
    // the origin) is outside the volume; positive runs to coneLength.
    vec3   coneOrigin   = vec3(0.0f, 0.0f, 0.0f);
    vec3   coneAxis     = vec3(0.0f, 0.0f, -1.0f);
    float  coneLength   = 1.0f;     // axial extent (world units)
    float  coneRadius   = 0.3f;     // radial extent at the bounding ellipsoid
    color3 coneInner    = color3(0.6f, 0.7f, 1.0f);   // hot core (blue-white)
    color3 coneOuter    = color3(1.0f, 0.4f, 0.05f);  // cool envelope (orange)
    float  coneIntensity = 50.0f;   // overall σe scale at the hot spot
    // Stratified samples for the volumetric emission integral. 4 keeps noise
    // bounded for typical flames; bump if the cone gradient looks dotty.
    int    coneEmissionSamples = 4;
    // Where along the axial fraction (0..1) the temperature peak lives.
    // 0.15 matches afterburner photos — hottest just downstream of the nozzle.
    float  conePeakAxial = 0.15f;
    // Sharpness of the axial gaussian. Higher = tighter hot spot.
    float  conePeakSharpness = 5.0f;
    // When true, coneOrigin / coneAxis are interpreted in the owning
    // SceneObject's *local* space and bake() applies the object's world
    // transform on top of viewMatrix — moving the SceneObject moves the
    // flame with it. When false (default for backward compat with existing
    // JSON scenes), coneOrigin / coneAxis are world-space and the flame
    // stays put even if the bounding mesh is moved.
    bool   coneFollowObject = false;

    // --- Phase 4: heat-haze (refractive shimmer) ---------------------------
    // Models the visual distortion behind a jet engine / over hot tarmac:
    // hot air has slightly lower density and therefore lower refractive index,
    // so light rays bend toward higher-density regions (Schlieren effect /
    // 陽炎). When `heatHaze` is on, rays inside this volume are ray-marched
    // and gradually steered by -∇n along the path; absorption / scattering /
    // emission for the volume are bypassed (the visible glow lives in a
    // separate emissive medium if the scene wants both).
    //
    // Physics: n(p) = 1 + iorAmplitude · fbm(p). For real heated air,
    // (n−1) is ~1e-4 .. 1e-3, so the visible deflection is a few pixels
    // over a metre of path. iorAmplitude here is the *visible* knob — push
    // it 10×−100× for stylised exaggeration on stage shots.
    bool   heatHaze        = false;
    float  iorAmplitude    = 0.005f;   // (n − 1) at the noise peak
    float  iorFrequency    = 4.0f;     // octave-0 frequency, separate from densityField
    int    iorOctaves      = 3;        // 1..6
    float  iorGain         = 0.5f;
    float  iorLacunarity   = 2.0f;
    int    iorMarchSteps   = 16;       // ray-march resolution along the segment
    vec3   iorOffset       = vec3::zero;  // shift the noise field (cheap "animation" between frames)

    // --- Phase 3: heterogeneous density field ------------------------------
    // FBmNoise multiplies σa/σs/σe at every sample point by a scalar
    // fractal-Brownian-motion noise drawn from a hash-based 3D value-noise
    // basis. Frequency sets the world-space scale of the smallest octave;
    // octaves stack progressively halved-amplitude / lacunarity-scaled
    // copies for cloudlike detail. noiseGain biases the [0,1] output and
    // noiseBias offsets it before clamping — set noiseBias < 0 to carve
    // empty pockets ("wisps"), > 0 to fill in ambient density.
    DensityFieldMode densityField = DensityField_None;
    float  noiseFrequency = 1.0f;       // octave-0 frequency (cycles / world unit)
    int    noiseOctaves   = 4;          // 1..6 typical
    float  noiseGain      = 0.5f;       // amplitude ratio between octaves
    float  noiseLacunarity = 2.0f;      // frequency ratio between octaves
    float  noiseAmplitude = 1.0f;       // overall multiplier on the fBm output
    float  noiseBias      = 0.0f;       // additive offset before clamping
    vec3   noiseOffset    = vec3::zero; // world-space anchor for the noise field
    // σt upper bound used by delta tracking. Computed by prepare() from the
    // homogeneous σt and the fBm peak (noiseAmplitude*(1)+noiseBias clamped).
    // Conservative — overestimating only adds null-collision steps, not bias.
    color3 sigma_t_max     = color3::zero;
    float  sigma_t_max_hero = 0.0f;

    // Cached derivatives. Recompute via prepare() after any field change so
    // the renderer's hot path doesn't re-add per ray.
    color3 sigma_t = color3::zero;   // sigma_a + sigma_s, post-density
    color3 sigma_s_eff = color3::zero;
    color3 sigma_e_eff = color3::zero;
    float  sigma_t_hero = 0.0f;      // average channel for free-flight pdf
    vec3   coneAxisN = vec3(0.0f, 0.0f, -1.0f);  // normalised coneAxis (world)

    // Render-space cache. The renderer transforms scene geometry into the
    // BVH's "view space" (viewMatrix · modelMatrix) once per render(); cone
    // params authored in world space need the same transform applied so the
    // ray-marched evaluation hits the right region. bake() refreshes these
    // before tracePath sees the medium for the first time on a frame.
    vec3   coneOriginR = vec3::zero;
    vec3   coneAxisR   = vec3(0.0f, 0.0f, -1.0f);

    // --- Phase 5: swept-tube (poly-line) emission --------------------------
    // Authored polyline + per-point radius/t. Containing mesh (cube/cone)
    // is still the volume's physical boundary — pathPoints must lie inside
    // it in object space, otherwise the emission outside the bounding mesh
    // contributes nothing. Path emission uses the same stratified MC line
    // integral as Cone, just with a different σe(p) evaluator.
    std::vector<PathSample> pathPoints;
    std::vector<PathSample> pathPointsR;        // bake()-transformed copy
    bool   pathFollowObject  = false;            // mirrors coneFollowObject
    color3 pathInner         = color3(0.6f, 0.7f, 1.0f);
    color3 pathOuter         = color3(0.3f, 0.4f, 0.6f);
    float  pathIntensity     = 1.0f;
    // (1 - r/radius)^pathFalloffPower radial profile. >1 gives a softer,
    // more cloud-like edge; 1 is linear; 2 is the Cone-mode radial default.
    float  pathFalloffPower  = 2.0f;
    int    pathEmissionSamples = 6;

    HomogeneousMedium() { }
    HomogeneousMedium(const color3& sa, const color3& ss, float g = 0.0f, float density = 1.0f)
        : sigma_a(sa), sigma_s(ss), g(g), density(density) { prepare(); }

    void prepare();

    // Bake authored cone params into the renderer's view space. When
    // `coneFollowObject` is true, applies modelMatrix first (object-local
    // origin/axis follow the SceneObject); otherwise modelMatrix is
    // ignored and the authored params are treated as world-space. Origin
    // is a point (w=1), axis a direction (w=0) and renormalised. No-op
    // when the cone profile isn't in use. Call once per frame before
    // tracePath touches the medium.
    void bake(const Matrix4& viewMatrix, const Matrix4& modelMatrix);

    // The volumetric branch of tracePath fires when this returns true. A pure-
    // emission cone (σt=0) needs to be considered active too — otherwise the
    // legacy non-volumetric fast path runs and the flame disappears. Heat-
    // haze likewise activates the volumetric branch so the renderer can ray-
    // march and bend the path even when σ values are all zero (a pure
    // refractive shimmer volume has no extinction).
    inline bool isActive() const {
        if (this->sigma_t_hero > 0.0f) return true;
        if (this->emissionMode == EmissionMode_Cone && this->coneIntensity > 0.0f) return true;
        if (this->emissionMode == EmissionMode_Path
            && this->pathIntensity > 0.0f
            && this->pathPoints.size() >= 2) return true;
        if (this->heatHaze && this->iorAmplitude > 0.0f) return true;
        return this->sigma_e_eff != color3::zero;
    }

    // True when this volume should bend the ray instead of running the
    // standard scatter / emission integration for the segment. Mutually
    // exclusive in the renderer hot path with the σ-driven volumetric
    // branch (heat haze takes priority); the visible glow of a flame
    // belongs in a separate medium.
    inline bool isHeatHaze() const {
        return this->heatHaze && this->iorAmplitude > 0.0f && this->iorMarchSteps > 0;
    }

    // Beer-Lambert per-channel transmittance over a segment of length d.
    color3 transmittance(float d) const;

    // Closed-form integral of (Tr(s) · σe) ds over [0, d] for Constant mode.
    // For Cone mode, use emissionIntegralAlongRay() — the procedural σe(p)
    // can't be integrated analytically.
    color3 emissionAlongRay(float d) const;

    // Procedural σe at world-space point p. Returns sigma_e_eff for Constant,
    // the cone-evaluated emission for Cone. Density and coneIntensity are
    // baked into the result so callers don't reapply them.
    color3 emissionAt(const vec3& p) const;

    // Returns the fBm density modulator at p (render-space), or 1.0 when
    // densityField is None — caller multiplies σ values by this scalar.
    float densityAt(const vec3& p) const;

    // Heterogeneous-aware variants. Caller passes the homogeneous baseline σ;
    // these multiply by densityAt(p). Cheap inlines so the renderer can
    // branch on densityField mode at the hot path.
    inline color3 sigmaTAt(const vec3& p) const {
        return (this->densityField == DensityField_None) ? this->sigma_t : this->sigma_t * this->densityAt(p);
    }
    inline color3 sigmaSAt(const vec3& p) const {
        return (this->densityField == DensityField_None) ? this->sigma_s_eff : this->sigma_s_eff * this->densityAt(p);
    }

    inline bool isHeterogeneous() const { return this->densityField != DensityField_None; }

    // Delta-tracking free-flight for heterogeneous media. Walks exponential
    // steps at the σt_max_hero rate; at each candidate point, accepts as a
    // real collision with probability density(p)/peak. On accept, returns
    // outT = step distance, outDensity = density at the accept point, and
    // the caller treats it as a scatter/absorb event. On no-accept after
    // walking past maxT, returns false and the caller does a surface event.
    // Phase 3 simplification: per-channel transmittance to the surface on
    // the no-accept branch is left at 1 (slightly overestimates light
    // through absorbing-dominant heterogeneous media; cloud / fog look
    // correct because their transmittance is dominated by scattering decay
    // already integrated through the rejection loop).
    bool sampleDeltaTracking(const Ray& ray, float maxT,
                             float& outT, float& outDensity) const;

    // ∫₀^maxT Tr(s)·σe(p(s)) ds — stratified Monte Carlo when in Cone mode,
    // closed-form when in Constant mode. Caller passes the ray so we can
    // walk world-space points along the segment without re-deriving them.
    color3 emissionIntegralAlongRay(const Ray& ray, float maxT) const;

    // Sample a free-flight distance from p(t) = σt_hero · exp(-σt_hero · t).
    // Returned distance is unbounded (caller compares against surface t to
    // decide scatter vs surface event).
    float sampleFreeFlight(float u) const;

    // PDF of the free-flight sampler at distance t.
    inline float freeFlightPdf(float t) const {
        return this->sigma_t_hero * expf(-this->sigma_t_hero * t);
    }

    // Probability that the free-flight sample exceeds distance d (i.e. the
    // ray reaches the surface without scattering). Equals 1 - CDF(d) under
    // the hero-channel pdf above: exp(-σt_hero · d).
    inline float freeFlightSurvivalProb(float d) const {
        return expf(-this->sigma_t_hero * d);
    }

    // Refractive index n(p) at world-space point p. Returns 1.0 when heat
    // haze is off (used internally by the bender; callers should normally
    // check isHeatHaze() first to skip the work).
    float iorAt(const vec3& p) const;

    // ∇n at p via central differences. Step size adapts to iorFrequency so
    // the gradient reflects the noise's natural detail scale.
    vec3 iorGradientAt(const vec3& p) const;

    // Ray-march `length` units from `ray.origin` along the (initially
    // straight) direction, bending each step by the refractive-index gradient.
    // Returns the bent ray (origin = endpoint, dir = bent direction). Caller
    // continues tracing from this ray; the original segment's other side of
    // the bounding mesh is discovered by the next BVH query.
    Ray bendRay(const Ray& ray, float length) const;

    // Sample a scattered direction wi given incoming wo using the Henyey-
    // Greenstein phase function. (u1, u2) are uniform in [0, 1). Returns the
    // direction; PDF can be queried separately via phasePdf(wo, wi).
    vec3 samplePhase(const vec3& wo, float u1, float u2) const;

    // PDF (and value, since HG is normalised to integrate to 1) of the
    // Henyey-Greenstein phase function for cosTheta = dot(wo, wi).
    float phasePdf(const vec3& wo, const vec3& wi) const;
};

}

#endif /* __raygen_medium_h__ */
