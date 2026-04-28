///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "bsdf.h"
#include "medium.h"
#include "rayrenderer.h"
#include "ugm/color.h"

namespace raygen {

// Geometric face normal of `tri` re-oriented to lie in the same hemisphere
// as the smooth shading normal `shadingN`. Used for self-intersection offsets
// (SurfaceRay) so the bias is perpendicular to the actual triangle plane,
// not the (potentially heavily tilted) interpolated shading normal — without
// this, a reflected ray sampled in the shading-normal hemisphere can still
// sit below the geometric plane and immediately re-hit the originating
// triangle. Most visible on Gerstner-displaced ocean meshes viewed at
// grazing angles, where the wave-bumps create steep shading-vs-geometric
// disagreement and BSDF paths get trapped bouncing between adjacent crests
// until MAX_TRACE_DEPTH / Russian roulette terminates them at throughput 0
// (the "black triangles at grazing" symptom).
inline vec3 geomNormal(const RenderMeshTriangle& tri, const vec3& shadingN) {
    const vec3& gn = tri.ti.normalizedpd;
    return (dot(gn, shadingN) >= 0.0f) ? gn : -gn;
}

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
    const Ray ray = SurfaceRay(interInfo.hit, dir, geomNormal(*interInfo.triangle, param.vi.normal));

    color3 albedo(1.0f, 1.0f, 1.0f);
    if (renderer.settings.enableColorSampling) {
        albedo = m.color;
        if (m.texture != NULL) {
            albedo *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
        }
    }

    const color3 savedT = param.throughput;
    const float savedBsdfPdf = param.bsdfSampledPdf;
    param.throughput *= albedo;
    // pdf of the cos-weighted sampled direction = cosθ/π. Pass it down so the
    // next hit (emission MIS) and tracePath (envmap miss MIS) can weight the
    // BSDF-strategy side of the power heuristic against the area-light / env
    // luminance-IS strategies used by traceLight / traceEnvmapLight.
    const float cosSampled = fmaxf(0.0f, dot(dir, param.vi.normal));
    param.bsdfSampledPdf = cosSampled * (float)(1.0 / M_PI);

    color3f color = renderer.tracePath(ray, (void*)&param)
                  + renderer.traceLight(interInfo.hit, param.vi.normal)
                  + renderer.traceEnvmapLight(interInfo.hit, param.vi.normal, param.bsdfSampledPdf);

    // Phase 4: NEE against emissive participating media. Equiangular line
    // sampling gives a 1/r²-weighted draw on the cone axis, then we evaluate
    // σe at the sample point (already medium-self-attenuated) and add the
    // Lambertian direct term L = (albedo/π) · Le · cosθ / pdfω. albedo is
    // applied at return so we just multiply by cosθ/(π·pdf) here.
    {
        vec3 vDir; float vDist; float vPdf = 0.0f; color3 vLe;
        if (renderer.sampleVolumeLightForNEE(interInfo.hit, param.vi.normal,
                                              vDir, vDist, vPdf, vLe) && vPdf > 0.0f) {
            const float cosL = fmaxf(0.0f, dot(vDir, param.vi.normal));
            color += vLe * (cosL * (float)(1.0 / M_PI) / vPdf);
        }
    }

    param.throughput = savedT;
    param.bsdfSampledPdf = savedBsdfPdf;

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

    const color3 F0 = (m.metallic >= 1.0f) ? albedo
                    : (albedo * m.metallic + color3(0.04f, 0.04f, 0.04f) * (1.0f - m.metallic));
    auto schlickF = [&](float VdotH) {
        const float a1 = fmaxf(0.0f, 1.0f - VdotH);
        const float a5 = a1 * a1 * a1 * a1 * a1;
        return F0 + (color3(1.0f, 1.0f, 1.0f) - F0) * a5;
    };

    // Smooth material fast path: treat ~0 roughness as a delta mirror. The
    // BSDF pdf is a Dirac delta so NEE can't hit this lobe, and the emission
    // hit at the reflected direction needs full radiance weight (no MIS) —
    // advertise bsdfSampledPdf = 0 so the next hit skips the BSDF-side MIS
    // weight.
    if (m.roughness < 1e-3f) {
        const vec3 gN = geomNormal(*interInfo.triangle, normal);
        // Reflect off the shading normal; if the result dives below the
        // geometric plane (smooth-shading vs flat-geometry disagreement —
        // common on Gerstner-displaced ocean meshes where vertex-normal
        // interpolation tilts the shading normal almost parallel to the
        // grazing camera ray), fall back to reflecting off the geometric
        // face normal so the bounce stays in the upper hemisphere instead
        // of plunging down through the surface and sampling the dark
        // hemisphere of the envmap (the "black wave triangles" symptom).
        vec3 r = reflect(inDir, normal);
        if (dot(r, gN) <= 0.0f) {
            r = reflect(inDir, gN);
        }
        const float cosI = fmaxf(0.0f, -dot(inDir, normal));
        const color3 F = schlickF(cosI);

        const color3 savedT = param.throughput;
        const float savedPdf = param.bsdfSampledPdf;
        param.throughput *= F;
        param.bsdfSampledPdf = 0.0f;
        const color3 incoming = renderer.tracePath(SurfaceRay(interInfo.hit, r, gN), (void*)&param);
        param.throughput = savedT;
        param.bsdfSampledPdf = savedPdf;
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

    // View direction in the local frame (z = normal).
    const vec3 V = -inDir;
    const vec3 Vlocal(dot(V, t), dot(V, b), dot(V, normal));
    if (Vlocal.z <= 0.0f) return color3::zero;  // back-face

    const float G1v = smithG1(Vlocal, ax, ay);

    // Evaluate (f·cos_L, pdf_bsdf) at an arbitrary world-space outgoing dir.
    // Returns false when the geometry is invalid (below-surface / degenerate
    // half-vector). Shared between area-light and envmap NEE.
    auto evalGlossy = [&](const vec3& dirWorld, color3& fCos, float& pdfBsdf) -> bool {
        const vec3 Llocal(dot(dirWorld, t), dot(dirWorld, b), dot(dirWorld, normal));
        if (Llocal.z <= 0.0f) return false;
        vec3 Hlocal = Vlocal + Llocal;
        const float hlen2 = Hlocal.x*Hlocal.x + Hlocal.y*Hlocal.y + Hlocal.z*Hlocal.z;
        if (hlen2 <= 0.0f) return false;
        Hlocal = Hlocal * (1.0f / sqrtf(hlen2));
        if (Hlocal.z <= 0.0f) return false;
        const float VdotH = fmaxf(0.0f, dot(Vlocal, Hlocal));
        if (VdotH <= 0.0f) return false;
        const float hx_ax = Hlocal.x / ax;
        const float hy_ay = Hlocal.y / ay;
        const float denom = hx_ax * hx_ax + hy_ay * hy_ay + Hlocal.z * Hlocal.z;
        const float D = 1.0f / ((float)M_PI * ax * ay * denom * denom);
        const float G1l = smithG1(Llocal, ax, ay);
        const color3 F = schlickF(VdotH);
        // f·cos_L = F·D·G2 / (4·V.z) — the L.z cancels the 1/L.z in the BRDF.
        fCos = F * (D * G1v * G1l / (4.0f * Vlocal.z));
        pdfBsdf = G1v * D / (4.0f * Vlocal.z);
        return true;
    };

    color3 direct = color3::zero;

    // Area-light NEE with MIS against the BSDF strategy. This is the payoff
    // path for scenes where the light is only reachable by a glossy bounce
    // from the eye — BSDF-only sampling rarely lines up with the emitter, so
    // without NEE those pixels remain as high-variance fireflies forever.
    {
        vec3 lDir; float pdfLight = 0.0f; color3 Le;
        if (renderer.sampleAreaLightForNEE(interInfo.hit, normal, lDir, pdfLight, Le)) {
            color3 fCos; float pdfBsdf = 0.0f;
            if (evalGlossy(lDir, fCos, pdfBsdf)) {
                const float pl2 = pdfLight * pdfLight;
                const float pb2 = pdfBsdf * pdfBsdf;
                const float wLight = pl2 / (pl2 + pb2);
                direct += Le * fCos * (wLight / pdfLight);
            }
        }
    }

    // Envmap NEE with MIS (same structure as area-light NEE).
    {
        vec3 eDir; float pdfEnv = 0.0f; color3 Li;
        if (renderer.sampleEnvmapForNEE(interInfo.hit, normal, eDir, pdfEnv, Li)) {
            color3 fCos; float pdfBsdf = 0.0f;
            if (evalGlossy(eDir, fCos, pdfBsdf)) {
                const float pe2 = pdfEnv * pdfEnv;
                const float pb2 = pdfBsdf * pdfBsdf;
                const float wEnv = pe2 / (pe2 + pb2);
                direct += Li * fCos * (wEnv / pdfEnv);
            }
        }
    }

    // Phase 4: volumetric-emitter NEE. Equiangular line sampling produces
    // a 1/r²-weighted draw on the cone axis. We don't pair this with a BSDF
    // strategy (a glossy lobe is unlikely to randomly hit the line) so MIS
    // weight is 1 — slight overestimate is bounded by the firefly clamp.
    {
        vec3 vDir; float vDist; float vPdf = 0.0f; color3 vLe;
        if (renderer.sampleVolumeLightForNEE(interInfo.hit, normal,
                                              vDir, vDist, vPdf, vLe) && vPdf > 0.0f) {
            color3 fCos; float pdfBsdf = 0.0f;
            if (evalGlossy(vDir, fCos, pdfBsdf)) {
                direct += vLe * fCos * (1.0f / vPdf);
            }
        }
    }

    // BSDF sampling: visible-normal sample → half vector, then reflect.
    const vec3 H_local = sampleGGXVNDF(Vlocal, ax, ay, randomValue(), randomValue());
    vec3 L_local = H_local * (2.0f * dot(Vlocal, H_local)) - Vlocal;
    if (L_local.z <= 0.0f) return direct;  // below surface — direct-only

    const vec3 L = t * L_local.x + b * L_local.y + normal * L_local.z;

    // Geometric-plane sanity: the VNDF-sampled L can be above the shading
    // plane yet below the geometric face plane when the smooth shading
    // normal disagrees with the flat triangle (curved low-poly meshes
    // viewed at grazing). Trace below the geom plane on those samples
    // would re-enter the triangle and either self-hit or bounce into
    // the dark hemisphere of the envmap. Drop the BSDF contribution and
    // keep just the direct-lighting term — NEE has already been added
    // above for the area / envmap / volume lobes.
    const vec3 gN = geomNormal(*interInfo.triangle, normal);
    if (dot(L, gN) <= 0.0f) return direct;

    const float VdotH = fmaxf(0.0f, dot(Vlocal, H_local));
    const color3 F = schlickF(VdotH);

    // VNDF sampling estimator: weight = F · G1(outgoing). The V-side G1 is
    // cancelled by the VNDF pdf.
    const float G1o = smithG1(L_local, ax, ay);
    const color3 weight = F * G1o;

    // pdf of the sampled direction, handed to the next hit so emission MIS
    // and envmap-miss MIS can apply the complementary BSDF-side weight.
    const float hx_ax = H_local.x / ax;
    const float hy_ay = H_local.y / ay;
    const float denomBS = hx_ax * hx_ax + hy_ay * hy_ay + H_local.z * H_local.z;
    const float D_bs = 1.0f / ((float)M_PI * ax * ay * denomBS * denomBS);
    const float pdfBsdfSampled = G1v * D_bs / (4.0f * Vlocal.z);

    const color3 savedT = param.throughput;
    const float savedPdf = param.bsdfSampledPdf;
    param.throughput *= weight;
    param.bsdfSampledPdf = pdfBsdfSampled;
    const color3 incoming = renderer.tracePath(SurfaceRay(interInfo.hit, L, gN), (void*)&param);
    param.throughput = savedT;
    param.bsdfSampledPdf = savedPdf;

    return direct + incoming * weight;
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

    const bool pickReflect = (randomValue() < fresnel);
    vec3 dir = pickReflect
        ? reflect(inDir, normal)
        : refract(inDir, normal, ior);

    if (m.roughness > 0.0f) {
        dir = (dir + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    // Geometric-plane sanity for the reflect branch — the smooth shading
    // normal can tilt enough on a wave mesh that the mirror reflection
    // dives below the geometric face plane and self-intersects (or, with
    // an envmap that has a dark ground hemisphere, samples the floor and
    // turns the triangle black). Re-reflect off the geometric face normal
    // in that case so the bounce stays in the upper hemisphere. Refract
    // is left alone — refraction is supposed to cross the interface.
    const vec3 gN = geomNormal(*interInfo.triangle, normal);
    if (pickReflect && dot(dir, gN) <= 0.0f) {
        dir = reflect(inDir, gN);
    }

    const color3 savedT = param.throughput;
    const float savedPdf = param.bsdfSampledPdf;
    const HomogeneousMedium* savedMedium = param.currentMedium;
    param.throughput *= m.color;
    // Refraction picks via a stochastic delta Fresnel split — both branches
    // are effectively delta lobes. The next hit should get full contribution
    // with no BSDF-side MIS pair.
    param.bsdfSampledPdf = 0.0f;

    // Swap the path's current medium when the ray actually crossed the
    // interface (the refract branch — reflection bounces back on the same
    // side). Reflection flips dot(dir, normal); refraction preserves its
    // sign, so the product with dot(inDir, normal) stays positive only on
    // the refract branch. Phase 1 does no nesting: exiting an object
    // always returns to the scene's global medium, so two adjacent water
    // cubes can't currently share their interior — revisited when the
    // medium-stack lands.
    const float dotIn = dot(inDir, normal);
    const float dotOut = dot(dir, normal);
    const bool refracted = dotIn * dotOut > 0.0f;
    if (refracted) {
        const bool entering = dotIn < 0.0f;
        if (entering) {
            const HomogeneousMedium* interior = obj.interiorMedium;
            if (interior != NULL) param.currentMedium = interior;
        } else {
            const Scene* sc = renderer.getScene();
            param.currentMedium = (sc != NULL) ? sc->globalMedium : NULL;
        }
    }

    const color3f color = renderer.tracePath(SurfaceRay(interInfo.hit, dir, gN), (void*)&param);

    param.throughput = savedT;
    param.bsdfSampledPdf = savedPdf;
    param.currentMedium = savedMedium;

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
    const float savedPdf = param.bsdfSampledPdf;
    param.throughput *= m.color;
    param.bsdfSampledPdf = 0.0f;  // delta refraction lobe

    const color3f color = renderer.tracePath(SurfaceRay(interInfo.hit, r, geomNormal(*interInfo.triangle, normal)), (void*)&param);

    param.throughput = savedT;
    param.bsdfSampledPdf = savedPdf;

    return color * m.color;
}

color3 TransparencyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const color3 savedT = param.throughput;
    const HomogeneousMedium* savedMedium = param.currentMedium;
    param.throughput *= m.transparency;

    // Transparency is a straight-through pass, but it still crosses an object
    // boundary — so the participating-medium state needs to flip just like in
    // RefractionShader. Without this, putting a flame inside a transparent
    // bounding cube wouldn't activate the interior medium (Phase 1 only
    // swapped on refraction). Reflection isn't possible on this branch since
    // the ray direction is preserved; we always take the "through" side.
    const float dotIn = dot(param.inray.dir, param.vi.normal);
    const bool entering = dotIn < 0.0f;
    if (entering) {
        const HomogeneousMedium* interior = obj.interiorMedium;
        if (interior != NULL) param.currentMedium = interior;
    } else {
        const Scene* sc = renderer.getScene();
        param.currentMedium = (sc != NULL) ? sc->globalMedium : NULL;
    }

    const color3 color = renderer.tracePath(SurfaceRay(interInfo.hit, param.inray.dir, geomNormal(*interInfo.triangle, param.vi.normal)), (void*)&param);

    param.throughput = savedT;
    param.currentMedium = savedMedium;

    return color * m.transparency;
}

color3 AnisotropicShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3& normal = param.vi.normal;

    const vec3 dir = randomRayInHemisphere(normal);
    const Ray ray = SurfaceRay(interInfo.hit, dir, geomNormal(*interInfo.triangle, normal));

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
