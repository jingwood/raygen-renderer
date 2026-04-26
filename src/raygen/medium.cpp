///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "medium.h"

#include <math.h>

#include "ugm/functions.h"

namespace raygen {

void HomogeneousMedium::prepare() {
    const float d = fmaxf(0.0f, this->density);
    this->sigma_s_eff = this->sigma_s * d;
    this->sigma_e_eff = this->sigma_e * d;
    const color3 sa = this->sigma_a * d;
    this->sigma_t = sa + this->sigma_s_eff;
    // Hero channel for free-flight sampling: average of σt over R/G/B. The
    // per-channel weight Tr(t)/p_hero(t) corrects the spectral mismatch in
    // tracePath, so any positive scalar works — average minimises variance
    // when σt is roughly grey, which covers fog/smoke/clouds.
    this->sigma_t_hero = (this->sigma_t.r + this->sigma_t.g + this->sigma_t.b) * (1.0f / 3.0f);
    // Cache the normalised cone axis so emissionAt doesn't normalise per
    // sample. Falls back to -Z when the user gave a degenerate axis.
    const float axisLen2 = this->coneAxis.x * this->coneAxis.x
                         + this->coneAxis.y * this->coneAxis.y
                         + this->coneAxis.z * this->coneAxis.z;
    this->coneAxisN = (axisLen2 > 1e-12f)
        ? this->coneAxis * (1.0f / sqrtf(axisLen2))
        : vec3(0.0f, 0.0f, -1.0f);
    // Default the render-space cache to the authored values so emissionAt
    // works even when the renderer never calls bake() (single-frame CLI run
    // with the legacy view-matrix === identity path stays correct).
    this->coneOriginR = this->coneOrigin;
    this->coneAxisR   = this->coneAxisN;
}

void HomogeneousMedium::bake(const Matrix4& viewMatrix) {
    if (this->emissionMode != EmissionMode_Cone) return;
    // Treat coneOrigin as a point (w=1), coneAxis as a direction (w=0). The
    // codebase uses row-vector × matrix multiplication (vec4 op*(Matrix4) is
    // defined that way in ugm), so mirror the same convention here.
    const vec4 originW(this->coneOrigin.x, this->coneOrigin.y, this->coneOrigin.z, 1.0f);
    const vec4 axisW  (this->coneAxisN.x,  this->coneAxisN.y,  this->coneAxisN.z,  0.0f);
    const vec4 originV = originW * viewMatrix;
    const vec4 axisV   = axisW   * viewMatrix;
    this->coneOriginR = vec3(originV.x, originV.y, originV.z);
    const float al2 = axisV.x*axisV.x + axisV.y*axisV.y + axisV.z*axisV.z;
    this->coneAxisR  = (al2 > 1e-12f)
        ? vec3(axisV.x, axisV.y, axisV.z) * (1.0f / sqrtf(al2))
        : this->coneAxisN;
}

color3 HomogeneousMedium::transmittance(float d) const {
    if (d <= 0.0f) return color3(1.0f, 1.0f, 1.0f);
    return color3(expf(-this->sigma_t.r * d),
                  expf(-this->sigma_t.g * d),
                  expf(-this->sigma_t.b * d));
}

namespace {
// (1 - exp(-σ·d)) / σ, robust at σ → 0 (limit is d). Per-channel.
inline float emissionWeightChannel(float sigma_t, float d) {
    if (sigma_t < 1e-6f) return d;
    return (1.0f - expf(-sigma_t * d)) / sigma_t;
}
}

color3 HomogeneousMedium::emissionAlongRay(float d) const {
    if (d <= 0.0f) return color3::zero;
    return color3(this->sigma_e_eff.r * emissionWeightChannel(this->sigma_t.r, d),
                  this->sigma_e_eff.g * emissionWeightChannel(this->sigma_t.g, d),
                  this->sigma_e_eff.b * emissionWeightChannel(this->sigma_t.b, d));
}

color3 HomogeneousMedium::emissionAt(const vec3& p) const {
    if (this->emissionMode == EmissionMode_Constant) {
        return this->sigma_e_eff;
    }
    // Cone profile. Axial coordinate runs along coneAxisR from coneOriginR
    // (render-space cache populated by bake() once per frame so we don't pay
    // matrix mults per ray sample). Radial coordinate is the perpendicular
    // distance to the axis. Both normalised by the bounding extents so the
    // cone occupies u∈[0,1], v∈[0,1] — outside that ellipsoid σe is zero so
    // the flame doesn't glow past its envelope or contaminate the emission
    // integral with background contributions.
    const vec3 rel(p.x - this->coneOriginR.x,
                   p.y - this->coneOriginR.y,
                   p.z - this->coneOriginR.z);
    const float axialT = rel.x * this->coneAxisR.x
                       + rel.y * this->coneAxisR.y
                       + rel.z * this->coneAxisR.z;
    if (this->coneLength <= 0.0f) return color3::zero;
    const float u = axialT / this->coneLength;
    if (u < 0.0f || u > 1.0f) return color3::zero;

    const vec3 perp(rel.x - this->coneAxisR.x * axialT,
                    rel.y - this->coneAxisR.y * axialT,
                    rel.z - this->coneAxisR.z * axialT);
    const float radial2 = perp.x * perp.x + perp.y * perp.y + perp.z * perp.z;
    if (this->coneRadius <= 0.0f) return color3::zero;
    const float v2 = radial2 / (this->coneRadius * this->coneRadius);
    if (v2 > 1.0f) return color3::zero;

    // Axial gaussian peak at conePeakAxial (a parameter; 0.15 by default puts
    // the hottest spot just downstream of the nozzle, matching real
    // afterburner photos). Radial profile is a softer (1 − v²)² so the edge
    // fades smoothly to the background colour.
    const float dx = (u - this->conePeakAxial) * this->conePeakSharpness;
    const float axialFalloff = expf(-dx * dx);
    const float radialFalloff = (1.0f - v2) * (1.0f - v2);
    const float t = axialFalloff * radialFalloff;
    if (t <= 0.0f) return color3::zero;

    // Lerp inner→outer by hot-spot proximity, then scale by the same factor
    // so the emission magnitude tracks the colour shift — cooler edge ⇒
    // dimmer too, not just orange-on-blue artifacts at the boundary.
    const color3 col = this->coneInner * t + this->coneOuter * (1.0f - t);
    const float scale = t * this->coneIntensity * fmaxf(0.0f, this->density);
    return col * scale;
}

color3 HomogeneousMedium::emissionIntegralAlongRay(const Ray& ray, float maxT) const {
    if (this->emissionMode == EmissionMode_Constant) {
        return this->emissionAlongRay(maxT);
    }
    if (maxT <= 0.0f) return color3::zero;

    // Stratified MC: split [0, maxT] into N strata, jitter one sample per
    // stratum. The Tr·σe·dt accumulator gives an unbiased estimator of the
    // line integral; bumping coneEmissionSamples trades noise for cost.
    const int N = (this->coneEmissionSamples > 0) ? this->coneEmissionSamples : 1;
    const float dt = maxT / (float)N;
    color3 accum = color3::zero;
    for (int i = 0; i < N; i++) {
        const float jit = randomValue();
        const float t = ((float)i + jit) * dt;
        const vec3 p(ray.origin.x + ray.dir.x * t,
                     ray.origin.y + ray.dir.y * t,
                     ray.origin.z + ray.dir.z * t);
        const color3 sigmaE = this->emissionAt(p);
        if (sigmaE == color3::zero) continue;
        const color3 Tr = this->transmittance(t);
        accum.r += sigmaE.r * Tr.r * dt;
        accum.g += sigmaE.g * Tr.g * dt;
        accum.b += sigmaE.b * Tr.b * dt;
    }
    return accum;
}

float HomogeneousMedium::sampleFreeFlight(float u) const {
    if (this->sigma_t_hero <= 0.0f) return 1e30f;
    // Inverse CDF of σ·exp(-σ·t). Clamp u away from 1 to avoid log(0) → ∞.
    const float xi = fminf(0.9999999f, fmaxf(0.0f, u));
    return -logf(1.0f - xi) / this->sigma_t_hero;
}

namespace {
// Build an orthonormal basis around w using the branchless Frisvad method.
inline void coordSystem(const vec3& w, vec3& u, vec3& v) {
    if (fabsf(w.x) > fabsf(w.y)) {
        const float invLen = 1.0f / sqrtf(w.x * w.x + w.z * w.z);
        u = vec3(-w.z * invLen, 0.0f, w.x * invLen);
    } else {
        const float invLen = 1.0f / sqrtf(w.y * w.y + w.z * w.z);
        u = vec3(0.0f, w.z * invLen, -w.y * invLen);
    }
    v = cross(w, u);
}
}

vec3 HomogeneousMedium::samplePhase(const vec3& wo, float u1, float u2) const {
    // Henyey-Greenstein inverse CDF for cosθ:
    //   cosθ = (1 + g² − ((1 − g²) / (1 − g + 2g·u))²) / (2g)
    // Falls back to uniform-sphere sampling at g ≈ 0.
    float cosTheta;
    if (fabsf(this->g) < 1e-3f) {
        cosTheta = 1.0f - 2.0f * u1;
    } else {
        const float g = this->g;
        const float sqr = (1.0f - g * g) / (1.0f - g + 2.0f * g * u1);
        cosTheta = (1.0f + g * g - sqr * sqr) / (2.0f * g);
    }
    const float sinTheta = sqrtf(fmaxf(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = 2.0f * (float)M_PI * u2;

    // Build a frame around wo. Convention: scattered direction is sampled
    // relative to the *forward* (incoming travel) axis, so positive cosθ is
    // forward scattering — matches the HG convention with g > 0 = forward.
    vec3 t1, t2;
    coordSystem(wo, t1, t2);
    return (t1 * (sinTheta * cosf(phi)) + t2 * (sinTheta * sinf(phi)) + wo * cosTheta).normalize();
}

float HomogeneousMedium::phasePdf(const vec3& wo, const vec3& wi) const {
    const float cosTheta = dot(wo, wi);
    const float g = this->g;
    const float denom = 1.0f + g * g - 2.0f * g * cosTheta;
    // p(cosθ) = (1 - g²) / (4π · (1 + g² - 2g·cosθ)^(3/2))
    return (1.0f - g * g) * (float)(1.0 / (4.0 * M_PI)) / (denom * sqrtf(fmaxf(1e-8f, denom)));
}

}
