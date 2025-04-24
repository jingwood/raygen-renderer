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
    
//    const vec3 dir = randomRayInHemisphere(param.vi.normal);
    const vec3 dir = cosineWeightedDirection(param.vi.normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);
    
    color3f indirect = renderer.tracePath(ray, (void*)&param);
    color3f direct = renderer.traceLight(interInfo, param.vi);

    // Lambert拡散 + 拡散率
    color3f color = (indirect + direct) * (m.color * m.diffuse) * (1.0f / M_PI);

    // テクスチャがある場合は適用
    if (renderer.settings.enableColorSampling && m.texture != NULL) {
        color *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
    }
    
    return color;
}

color3 EmissionShader::shade(BSDFParam& param) {
    const RayTriangleIntersectionInfo& interInfo = param.interInfo;
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    // 面がカメラ方向に向いているか確認
    const vec3 viewDir = normalize(param.inray.origin - interInfo.hit);
    const float cosTheta = fmax(dot(param.vi.normal, viewDir), 0.0f);

    return m.color * m.emission * cosTheta;
}

inline float fresnelSchlick(float cosTheta, float refractiveIndex) {
    float r0 = (1.0f - refractiveIndex) / (1.0f + refractiveIndex);
    r0 = r0 * r0;
    return r0 + (1.0f - r0) * powf(1.0f - cosTheta, 5.0f);
}

color3 GlossyShader::shade(BSDFParam& param) {
//    const RayRenderer& renderer = param.renderer;
//
//    const SceneObject& obj = param.interInfo.triangle->object;
//    const Material& m = obj.material;
//
//    const vec3& normal = param.vi.normal;
//
//    vec3 r = reflect(param.inray.dir, normal);
//
//    if (m.roughness > 0.0f) {
//        r = (r + cosineWeightedDirection(normal) * m.roughness).normalize();
//    }
//
//    const color3f color = renderer.tracePath(ThicknessRay(param.interInfo.hit, r), (void*)&param);
//
//    return color * m.color;
    
    const RayRenderer& renderer = param.renderer;
    const SceneObject& obj = param.interInfo.triangle->object;
    const Material& m = obj.material;
    
    const vec3& normal = param.vi.normal;
    vec3 viewDir = param.inray.dir;

    // 理想的な反射ベクトル
    vec3 idealReflect = reflect(viewDir, normal);

    // フレネル係数の計算（Schlickの近似）
    float cosTheta = fmaxf(dot(normal, viewDir), 0.0f);
    float fresnel = fresnelSchlick(cosTheta, m.refractionRatio);

    // 反射ベクトルの roughness による拡散
    vec3 reflectDir = idealReflect;
    if (m.roughness > 0.0f) {
        reflectDir = (reflectDir + cosineWeightedDirection(normal) * m.roughness).normalize();
    }

    // 反射レイをトレース
    const color3f reflectedColor = renderer.tracePath(ThicknessRay(param.interInfo.hit, reflectDir), (void*)&param);

    // フレネルによる調整 + マテリアル色
    return reflectedColor * fresnel * m.color;
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
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    const Material& m = interInfo.triangle->object.material;

    vec3 normal = param.vi.normal;
    vec3 inDir = param.inray.dir;

    // 正確なFresnel係数を計算
    float cosTheta = clamp(dot(-inDir, normal), 0.0f, 1.0f);
    float fresnel = fresnelSchlick(cosTheta, m.refractionRatio);

    // 屈折レイの方向
    vec3 refractedDir = refract(inDir, normal, m.refractionRatio);
    if (m.roughness > 0.0f) {
        refractedDir = (refractedDir + cosineWeightedDirection(normal) * m.roughness).normalize();
    }
    
    // 屈折レイをトレース
    color3 refractedColor = renderer.tracePath(ThicknessRay(interInfo.hit, refractedDir), (void*)&param);

    // 反射レイをトレース
    vec3 reflectedDir = reflect(inDir, normal);
    color3 reflectedColor = renderer.tracePath(ThicknessRay(interInfo.hit, reflectedDir), (void*)&param);
    
    // Fresnelブレンド + 色補正
//    color3 finalColor = (refractedColor * (1.0f - fresnel) + reflectedColor * fresnel) * m.color;

   return (refractedColor * (1.0f - fresnel) + reflectedColor * fresnel) * m.color * 1.5f; // 明度補正
    
    // 反射率1や0でもゼロにはならないように調整（数値誤差対策）
//    return finalColor;
//    return clamp(finalColor, 0.0f, 1.0f);
}

color3 GlassShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& interInfo = param.interInfo;
    
    const SceneObject& obj = interInfo.triangle->object;
    const Material& m = obj.material;
    
    vec3 normal = param.vi.normal;
    
    vec3 dir = refract(param.inray.dir, normal, m.refractionRatio);
    
    if (m.roughness > 0.0f) {
        dir = (dir + cosineWeightedDirection(normal) * m.roughness).normalize();
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
    
    const vec3& normal = param.vi.normal;
    
    const vec3 dir = cosineWeightedDirection(normal);
    const Ray ray = ThicknessRay(interInfo.hit, dir);
    
    color3 color = renderer.tracePath(ray, (void*)&param)
        + renderer.traceLight(interInfo, param.vi);
    
    if (renderer.settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(param.vi.uv * m.texTiling).rgb;
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
