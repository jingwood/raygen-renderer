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

color3 DiffuseShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    const vec3 dir = randomRayInHemisphere(param.hi.normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);
    
    color3f color = renderer.tracePath(ray, (void*)&param) + renderer.traceLight(interInfo, param.hi);
    
    if (renderer.settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(param.hi.uv * m.texTiling).rgb;
        }
    }
    
    return color;
}

color3 EmissionShader::shade(BSDFParam& param) {
    const RayTriangleIntersectionInfo& interInfo = param.interInfo;
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    // 面がカメラ方向に向いているか確認
    const vec3 viewDir = normalize(param.inray.origin - interInfo.hit);
    const float cosTheta = fmax(dot(param.hi.normal, viewDir), 0.0f);

    return m.color * m.emission * cosTheta;
}

color3 GlossyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    
    const SceneObject& obj = param.interInfo.triangle->object;
    const Material& m = obj.material;
    
    const vec3& normal = param.hi.normal;
    
    vec3 r = reflect(param.inray.dir, normal);
    
    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }
    
    const color3f color = renderer.tracePath(ThicknessRay(param.interInfo.hit, r), (void*)&param);
    
    return color * m.color;
}

inline float fresnelSchlick(float cosTheta, float refractiveIndex) {
    float r0 = (1.0f - refractiveIndex) / (1.0f + refractiveIndex);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * powf(1.0f - cosTheta, 5.0f);
}

inline vec3 refract(const vec3& d, const vec3& normal, float eta) {
    float cosi = clamp(dot(d, normal), -1.0f, 1.0f);
    float etai = 1.0f, etat = eta;
    vec3 n = normal;
    if (cosi < 0) {
        cosi = -cosi;
    } else {
        std::swap(etai, etat);
        n = -normal;
    }
    float etaRatio = etai / etat;
    float k = 1.0f - etaRatio * etaRatio * (1.0f - cosi * cosi);
    if (k < 0.0f) {
        return reflect(d, normal);  // 全反射
    } else {
        return normalize(d * etaRatio + n * (etaRatio * cosi - sqrtf(k)));
    }
}

color3 RefractionShader::shade(BSDFParam& param) {
//    const RayRenderer& renderer = param.renderer;
//    const auto& interInfo = param.interInfo;
//    
//    const SceneObject& obj = interInfo.triangle->object;
//    const Material& m = obj.material;
//    
//    vec3 normal = param.hi.normal;
//    
//    vec3 r = refract(param.inray.dir, normal, m.refractionRatio);
//    
//    if (m.roughness > 0.0f) {
//        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
//    }
//    
//    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, r), (void*)&param);
//    
//    return color * m.color;
    
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    const Material& m = interInfo.triangle->object.material;

    vec3 normal = param.hi.normal;
    vec3 inDir = param.inray.dir;

    float cosTheta = fmaxf(0.0f, dot(-inDir, normal));
    float fresnel = fresnelSchlick(cosTheta, m.refractionRatio);

    vec3 refractedDir = refract(inDir, normal, m.refractionRatio);
    if (m.roughness > 0.0f) {
        refractedDir = (refractedDir + randomRayInHemisphere(normal) * m.roughness).normalize();
    }

    // 屈折レイをトレース
    color3 refractedColor = renderer.tracePath(ThicknessRay(interInfo.hit, refractedDir), (void*)&param);

    // 反射レイをトレース
    vec3 reflectedDir = reflect(inDir, normal);
    color3 reflectedColor = renderer.tracePath(ThicknessRay(interInfo.hit, reflectedDir), (void*)&param);

    // フレネルでブレンド
    color3 finalColor = refractedColor * (1.0f - fresnel) + reflectedColor * fresnel;
    return finalColor * m.color;
}

color3 GlassShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    vec3 normal = param.hi.normal;
    
    vec3 dir = refract(param.inray.dir, normal, m.refractionRatio);
    
    if (m.roughness > 0.0f) {
        dir = (dir + randomRayInHemisphere(normal) * m.roughness).normalize();
    }
    
    const color3f color = renderer.tracePath(ThicknessRay(interInfo.hit, dir), (void*)&param);
    
    return color * m.color;
}

color3 TransparencyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    return (renderer.tracePath(ThicknessRay(interInfo.hit, param.inray.dir), (void*)&param)) * m.transparency;
}

color3 AnisotropicShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    const vec3& normal = param.hi.normal;
    
    const vec3 dir = randomRayInHemisphere(normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);
    
    color3 color = renderer.tracePath(ray, (void*)&param)
        + renderer.traceLight(interInfo, param.hi);
    
    if (renderer.settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(param.hi.uv * m.texTiling).rgb;
        }
    }
    
    return color;
}

color3 MixShader::shade(BSDFParam& param) {
    const auto& srcrmi = param.interInfo;
    
    const SceneObject& obj = srcrmi.triangle->object;
    const Material& m = obj.material;
    
    color3 color;
    
//    const float diffuse = 1.0f - m.glossy - m.refraction;
//    
//    if (diffuse > 0.00001f) {
//        color += diffuseShader.shade(param) * diffuse;
//    }
//    
//    if (m.glossy > 0.00001f) {
//        color += glossyShader.shade(param) * m.glossy;
//    }
//    
//    if (m.refraction > 0.00001f) {
//        color += refractionShader.shade(param) * m.refraction;
//    }
    
    //1. Fresnel反射を考慮
//    •    物理的に、入射角度によって反射と屈折の比率が変化する。
//    •    もしガラスや水のような質感を目指すなら：
//    float fresnel = fresnelSchlick(dot(viewDir, normal), ior);
//    color3 reflected = glossyShader.shade(param);
//    color3 refracted = refractionShader.shade(param);
//    color += reflected * fresnel + refracted * (1.0f - fresnel);
    
    float sum = m.diffuse + m.glossy + m.refraction;
    if (sum > 0.0f) {
        float diffuseWeight = m.diffuse / sum;
        float glossyWeight = m.glossy / sum;
        float refractionWeight = m.refraction / sum;
        
        float r = randomValue();
        if (r < diffuseWeight) {
            color = diffuseShader.shade(param) / diffuseWeight;
        } else if (r < diffuseWeight + glossyWeight) {
            color = glossyShader.shade(param) / glossyWeight;
        } else {
            color = refractionShader.shade(param) / refractionWeight;
        }
    }
    
    return color;
}

}
