///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "bsdf.h"
#include "rayrenderer.h"
#include "ugm/color.h"

namespace raygen {

inline float fresnelSchlick(float cosTheta, float refractiveIndex) {
    float r0 = (1.0f - refractiveIndex) / (1.0f + refractiveIndex);
    r0 = r0 * r0;
    const float m = 1.0f - cosTheta;
    return r0 + (1.0f - r0) * (m * m) * (m * m) * m;
}

inline vec3 refract(const vec3& d, const vec3& normal, float r = 1.45f) {
    const vec3 nl = dot(d, normal) < 0 ? normal : -normal;
    const bool into = dot(nl, normal) > 0;
    if (into) r = 1.0f / r;

    const float c = dot(d, nl);
    const float t = 1.0f - r * r * (1.0f - c * c);

    if (t < 0) {
        return reflect(d, normal);
    }

    return normalize(d * r - normal * ((into ? 1 : -1) * (c * r + sqrtf(t))));
}

color3 DiffuseShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    // Cosine-weighted hemisphere sampling: p(ω) = cos(θ)/π. For a Lambertian
    // BRDF albedo/π the MC estimator simplifies to albedo * L(ω), i.e. just
    // multiply the incoming radiance by surface color — no extra cos/π factors
    // needed in the shader (the 1/π is already in traceLight's direct term).
    const vec3 dir = cosineWeightedDirection(param.vi.normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);

    color3 albedo(1.0f, 1.0f, 1.0f);
    if (renderer.settings.enableColorSampling) {
        albedo = m.color;
        if (m.texture != NULL) {
            albedo *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
        }
    }

    const color3 savedT = param.throughput;
    const bool savedMIS = param.misDiffuse;
    const vec3 savedMISNormal = param.misNormal;
    const float savedEnvBsdfPdf = param.misEnvBsdfPdf;
    param.throughput *= albedo;
    // Advertise to the next hit that we sampled a cos-weighted diffuse lobe,
    // so it can MIS-weight any emission it finds against the shadow-ray
    // strategy used by traceLight.
    param.misDiffuse = true;
    param.misNormal = param.vi.normal;
    // pdf of the cos-weighted sampled direction = cosθ/π. Pass it down so
    // tracePath can MIS-weight envmap hits against the luminance-IS strategy
    // used by traceEnvmapLight.
    const float cosSampled = fmaxf(0.0f, dot(dir, param.vi.normal));
    param.misEnvBsdfPdf = cosSampled * (float)(1.0 / M_PI);

    color3f color = renderer.tracePath(ray, (void*)&param)
                  + renderer.traceLight(interInfo.hit, param.vi.normal)
                  + renderer.traceEnvmapLight(interInfo.hit, param.vi.normal, param.misEnvBsdfPdf);

    param.throughput = savedT;
    param.misDiffuse = savedMIS;
    param.misNormal = savedMISNormal;
    param.misEnvBsdfPdf = savedEnvBsdfPdf;

    return color * albedo;
}

color3 EmissionShader::shade(BSDFParam& param) {
    const RayTriangleIntersectionInfo& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3 lightray = interInfo.hit - param.inray.origin;

    const float dist = powf(lightray.length(), -2.0f);

    return m.color * m.emission
        * dist
        * fmax(dot(lightray, -param.vi.normal), 0.0f);
}

namespace {
// Build a tangent frame from the shading normal. The tangent direction is
// arbitrary for isotropic BRDFs; for the anisotropic case the user-supplied
// `anisoRotation` rotates this in-plane.
inline void buildTangentFrame(const vec3& n, vec3& t, vec3& b) {
    const vec3 up = (fabsf(n.y) > 0.99f) ? vec3(1.0f, 0.0f, 0.0f) : vec3(0.0f, 1.0f, 0.0f);
    t = cross(up, n).normalize();
    b = cross(n, t);
}

// Sample a microfacet normal from the GGX Distribution of Visible Normals,
// given the view direction in the local shading frame (z = shading normal).
// Heitz 2018 — "Sampling the GGX Distribution of Visible Normals". Returns
// a unit half-vector h whose z ≥ 0 (upper hemisphere).
inline vec3 sampleGGXVNDF(const vec3& Ve, float ax, float ay, float u1, float u2) {
    // Stretch view into the iso hemisphere.
    vec3 Vh = vec3(ax * Ve.x, ay * Ve.y, Ve.z).normalize();

    // Orthonormal basis (Frisvad-style when Vh.z ≈ 1).
    const float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    vec3 T1 = (lensq > 0.0f) ? vec3(-Vh.y, Vh.x, 0.0f) * (1.0f / sqrtf(lensq))
                              : vec3(1.0f, 0.0f, 0.0f);
    vec3 T2 = cross(Vh, T1);

    // Uniform sample on a disk; reshape to favour the view hemisphere.
    const float r = sqrtf(u1);
    const float phi = 2.0f * (float)M_PI * u2;
    const float t1 = r * cosf(phi);
    float t2 = r * sinf(phi);
    const float s = 0.5f * (1.0f + Vh.z);
    t2 = (1.0f - s) * sqrtf(fmaxf(0.0f, 1.0f - t1 * t1)) + s * t2;

    const vec3 Nh = T1 * t1 + T2 * t2 + Vh * sqrtf(fmaxf(0.0f, 1.0f - t1 * t1 - t2 * t2));

    // Un-stretch back to the anisotropic hemisphere.
    return vec3(ax * Nh.x, ay * Nh.y, fmaxf(0.0f, Nh.z)).normalize();
}

// Smith G1 for anisotropic GGX in the local shading frame. w.z > 0 assumed.
inline float smithG1(const vec3& w, float ax, float ay) {
    if (w.z <= 0.0f) return 0.0f;
    const float ax2 = (ax * w.x) * (ax * w.x);
    const float ay2 = (ay * w.y) * (ay * w.y);
    const float lambda = 0.5f * (sqrtf(1.0f + (ax2 + ay2) / (w.z * w.z)) - 1.0f);
    return 1.0f / (1.0f + lambda);
}
}

color3 GlossyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3& normal = param.vi.normal;
    const vec3& inDir = param.inray.dir;

    // Albedo (colour × texture) drives both the diffuse lobe (when not
    // metallic) and, for metals, the Fresnel F0.
    color3 albedo = m.color;
    if (renderer.settings.enableColorSampling && m.texture != NULL) {
        albedo *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
    }

    // Smooth material fast path: treat ~0 roughness as a delta mirror so we
    // don't divide by a vanishing D in the microfacet estimator.
    if (m.roughness < 1e-3f) {
        const vec3 r = reflect(inDir, normal);
        const float cosI = fmaxf(0.0f, -dot(inDir, normal));
        const color3 F0 = (m.metallic >= 1.0f) ? albedo
                        : (albedo * m.metallic + color3(0.04f, 0.04f, 0.04f) * (1.0f - m.metallic));
        const float m1 = 1.0f - cosI;
        const float m5 = m1 * m1 * m1 * m1 * m1;
        const color3 F = F0 + (color3(1.0f, 1.0f, 1.0f) - F0) * m5;

        const color3 savedT = param.throughput;
        param.throughput *= F;
        const color3 incoming = renderer.tracePath(ThicknessRay(interInfo.hit, r), (void*)&param);
        param.throughput = savedT;
        return incoming * F;
    }

    // Build a tangent frame for anisotropy. `t` rotates in the tangent plane
    // by anisoRotation so the user can orient the brush direction.
    vec3 t, b;
    buildTangentFrame(normal, t, b);
    if (fabsf(m.anisoRotation) > 0.0f) {
        const float a = m.anisoRotation * (float)(M_PI / 180.0);
        const float cosA = cosf(a), sinA = sinf(a);
        const vec3 t2 = t * cosA + b * sinA;
        const vec3 b2 = b * cosA - t * sinA;
        t = t2; b = b2;
    }

    // Anisotropic αx / αy split. clamp anisotropy to (-0.99, 0.99) so neither
    // axis collapses to zero width. Sign convention: aniso > 0 elongates the
    // highlight along the tangent direction, < 0 along bitangent — that means
    // a tighter distribution along tangent, so αx shrinks as aniso grows.
    const float aniso = fmaxf(-0.99f, fminf(0.99f, m.anisotropy));
    const float r2 = m.roughness * m.roughness;
    const float ax = fmaxf(1e-3f, r2 * (1.0f - aniso));
    const float ay = fmaxf(1e-3f, r2 * (1.0f + aniso));

    // Transform the view direction into the local frame (z = normal).
    const vec3 V = -inDir;  // toward camera from hit
    const vec3 Vlocal(dot(V, t), dot(V, b), dot(V, normal));
    if (Vlocal.z <= 0.0f) return color3::zero;  // back-face

    // Visible-normal sample → half vector, then reflect.
    const vec3 H_local = sampleGGXVNDF(Vlocal, ax, ay, randomValue(), randomValue());
    vec3 L_local = H_local * (2.0f * dot(Vlocal, H_local)) - Vlocal;
    if (L_local.z <= 0.0f) return color3::zero;  // below surface

    const vec3 L = t * L_local.x + b * L_local.y + normal * L_local.z;

    // Fresnel Schlick, with F0 colour-locked to albedo for metals.
    const float VdotH = fmaxf(0.0f, dot(Vlocal, H_local));
    const color3 F0 = (m.metallic >= 1.0f) ? albedo
                    : (albedo * m.metallic + color3(0.04f, 0.04f, 0.04f) * (1.0f - m.metallic));
    const float m1 = 1.0f - VdotH;
    const float m5 = m1 * m1 * m1 * m1 * m1;
    const color3 F = F0 + (color3(1.0f, 1.0f, 1.0f) - F0) * m5;

    // VNDF sampling estimator: weight = F · G1(outgoing). The V-side G1 is
    // cancelled by the VNDF pdf.
    const float G1o = smithG1(L_local, ax, ay);
    const color3 weight = F * G1o;

    const color3 savedT = param.throughput;
    param.throughput *= weight;
    const color3 incoming = renderer.tracePath(ThicknessRay(interInfo.hit, L), (void*)&param);
    param.throughput = savedT;

    return incoming * weight;
}

color3 RefractionShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3& normal = param.vi.normal;
    const vec3& inDir = param.inray.dir;

    // Stochastic Fresnel: pick reflection or refraction per sample based on the
    // Schlick factor, so on average the blend is energy-conserving without needing
    // two recursive tracePath calls (one branch = stack-safe). Use |cos θ| so
    // internal hits (ray travelling with the outward normal) get the same
    // incidence angle as the mirrored external hit — otherwise fresnel collapses
    // to 1.0 inside the medium and every ray is trapped by total reflection.
    const float cosTheta = fabsf(dot(inDir, normal));

    // Wavelength-dependent IOR (chromatic aberration). A path commits to a
    // single wavelength band at its first dispersive hit and every later
    // refractive interface on the path reuses that channel — otherwise the
    // per-interface channel masks multiply to zero and the glass goes black.
    // At the first CA hit we pick a channel uniformly, apply the matching
    // IOR offset, trace, and mask the return to that channel × 3 so the MC
    // estimator integrates over the three bands. Deeper CA hits just use
    // the stored channel's IOR and return full RGB (the outer mask clips).
    float ior = m.refractionRatio;
    color3 chanMask(1.0f, 1.0f, 1.0f);
    bool firstCAHit = false;
    if (m.chromaDispersion > 0.0f) {
        if (param.chromaChannel < 0) {
            param.chromaChannel = (int)fminf(2.0f, floorf(randomValue() * 3.0f));
            firstCAHit = true;
        }
        const int chan = param.chromaChannel;
        const float s = m.chromaDispersion;
        if (chan == 0) ior = m.refractionRatio * (1.0f - s);
        else if (chan == 2) ior = m.refractionRatio * (1.0f + s);
        // chan == 1 → base IOR
        if (firstCAHit) {
            if (chan == 0) chanMask = color3(3.0f, 0.0f, 0.0f);
            else if (chan == 1) chanMask = color3(0.0f, 3.0f, 0.0f);
            else chanMask = color3(0.0f, 0.0f, 3.0f);
        }
    }

    const float fresnel = fresnelSchlick(cosTheta, ior);

    vec3 dir = (randomValue() < fresnel)
        ? reflect(inDir, normal)
        : refract(inDir, normal, ior);

    if (m.roughness > 0.0f) {
        dir = (dir + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    const color3 savedT = param.throughput;
    param.throughput *= m.color;

    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, dir), (void*)&param);

    param.throughput = savedT;

    return color * m.color * chanMask;
}

color3 GlassShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    vec3 normal = param.vi.normal;

    vec3 r = refract(param.inray.dir, normal, m.refractionRatio);

    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    const color3 savedT = param.throughput;
    param.throughput *= m.color;

    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, r), (void*)&param);

    param.throughput = savedT;

    return color * m.color;
}

color3 TransparencyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const color3 savedT = param.throughput;
    param.throughput *= m.transparency;

    const color3 color = renderer.tracePath(ThicknessRay(interInfo.hit, param.inray.dir), (void*)&param);

    param.throughput = savedT;

    return color * m.transparency;
}

color3 AnisotropicShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3& normal = param.vi.normal;

    const vec3 dir = randomRayInHemisphere(normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);

    color3 albedo(1.0f, 1.0f, 1.0f);
    if (renderer.settings.enableColorSampling) {
        albedo = m.color;
        if (m.texture != NULL) {
            albedo *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
        }
    }

    const color3 savedT = param.throughput;
    param.throughput *= albedo;

    color3 color = renderer.tracePath(ray, (void*)&param)
                 + renderer.traceLight(interInfo.hit, param.vi.normal);

    param.throughput = savedT;

    return color * albedo;
}

color3 MixShader::shade(BSDFParam& param) {
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    color3 color;

    const float diffuse = 1.0f - m.glossy - m.refraction;
    const color3 savedT = param.throughput;

    // Each child branch's recursive tracePath needs the MixShader-level weight
    // baked into the throughput so Russian Roulette sees the true contribution.
    // Restore between siblings so branches don't interfere.
    if (diffuse > 0.00001f) {
        param.throughput = savedT * diffuse;
        color += diffuseShader.shade(param) * diffuse;
    }

    if (m.glossy > 0.00001f) {
        param.throughput = savedT * m.glossy;
        color += glossyShader.shade(param) * m.glossy;
    }

    if (m.refraction > 0.00001f) {
        param.throughput = savedT * m.refraction;
        color += refractionShader.shade(param) * m.refraction;
    }

    param.throughput = savedT;
    return color;
}

}
