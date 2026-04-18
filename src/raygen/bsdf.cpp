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
    param.throughput *= albedo;

    color3f color = renderer.tracePath(ray, (void*)&param)
                  + renderer.traceLight(interInfo.hit, param.vi.normal);

    param.throughput = savedT;

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

    const color3 savedT = param.throughput;
    param.throughput *= m.color;

    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, r), (void*)&param);

    param.throughput = savedT;

    return color * m.color;
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
    // two recursive tracePath calls (one branch = stack-safe).
    const float cosTheta = clamp(-dot(inDir, normal), 0.0f, 1.0f);
    const float fresnel = fresnelSchlick(cosTheta, m.refractionRatio);

    vec3 dir = (randomValue() < fresnel)
        ? reflect(inDir, normal)
        : refract(inDir, normal, m.refractionRatio);

    if (m.roughness > 0.0f) {
        dir = (dir + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    const color3 savedT = param.throughput;
    param.throughput *= m.color;

    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, dir), (void*)&param);

    param.throughput = savedT;

    return color * m.color;
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
