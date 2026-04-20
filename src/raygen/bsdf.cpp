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

color3 GlossyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;

    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;

    const vec3& normal = param.vi.normal;

    vec3 r = reflect(param.inray.dir, normal);

    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    // Tint the glossy reflection with the albedo (colour × texture). Without
    // this, a textured glossy surface (e.g. checker floor with glossy > 0)
    // returns an untextured white reflection from MixShader's glossy branch
    // that washes out the pattern in the diffuse branch.
    color3 albedo = m.color;
    if (renderer.settings.enableColorSampling && m.texture != NULL) {
        albedo *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
    }

    const color3 savedT = param.throughput;
    param.throughput *= albedo;

    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, r), (void*)&param);

    param.throughput = savedT;

    return color * albedo;
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
