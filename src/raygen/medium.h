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
    enum EmissionMode {
        EmissionMode_Constant = 0,
        EmissionMode_Cone     = 1,
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

    HomogeneousMedium() { }
    HomogeneousMedium(const color3& sa, const color3& ss, float g = 0.0f, float density = 1.0f)
        : sigma_a(sa), sigma_s(ss), g(g), density(density) { prepare(); }

    void prepare();

    // Bake authored (world-space) cone params into the renderer's view space
    // by applying the supplied 4×4 column-major transform (viewMatrix is the
    // typical caller). Origin is treated as a point (w=1), axis as a
    // direction (w=0) and renormalised. No-op when the cone profile isn't in
    // use. Call once per frame before tracePath touches the medium.
    void bake(const Matrix4& viewMatrix);

    // The volumetric branch of tracePath fires when this returns true. A pure-
    // emission cone (σt=0) needs to be considered active too — otherwise the
    // legacy non-volumetric fast path runs and the flame disappears.
    inline bool isActive() const {
        if (this->sigma_t_hero > 0.0f) return true;
        if (this->emissionMode == EmissionMode_Cone && this->coneIntensity > 0.0f) return true;
        return this->sigma_e_eff != color3::zero;
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
