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
    const auto& rmi = param.rmi;
    
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    const vec3 dir = randomRayInHemisphere(param.hi.normal);
    const Ray ray = ThicknessRay(rmi.hit, dir);
    
    color3f color = renderer.tracePath(ray, (void*)&param) + renderer.traceLight(rmi, param.hi);
    
    if (renderer.settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(param.hi.uv * m.texTiling).rgb;
        }
    }
    
    return color;
}

color3 EmissionShader::shade(BSDFParam& param) {
    const RayMeshIntersection& rmi = param.rmi;
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    // 面がカメラ方向に向いているか確認
    const vec3 viewDir = normalize(param.inray.origin - rmi.hit);
    const float cosTheta = fmax(dot(param.hi.normal, viewDir), 0.0f);

    return m.color * m.emission * cosTheta;
}

color3 GlossyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& rmi = param.rmi;
    
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    const vec3& normal = param.hi.normal;
    
    vec3 r = reflect(param.inray.dir, normal);
    
    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }
    
    const color3f color = renderer.tracePath(ThicknessRay(rmi.hit, r), (void*)&param);
    
    return color * m.color;
}

color3 RefractionShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& rmi = param.rmi;
    
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    vec3 normal = param.hi.normal;
    
    vec3 r = refract(param.inray.dir, normal, m.refractionRatio);
    
    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }
    
    const color3f color = renderer.tracePath(ThicknessRay(rmi.hit, r), (void*)&param);
    
    return color * m.color;
}

color3 GlassShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& rmi = param.rmi;
    
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    vec3 normal = param.hi.normal;
    
    vec3 r = refract(param.inray.dir, normal, m.refractionRatio);
    
    if (m.roughness > 0.0f) {
        r = (r + randomRayInHemisphere(normal) * m.roughness).normalize();
    }
    
    const color3f color = renderer.tracePath(ThicknessRay(rmi.hit, r), (void*)&param);
    
    return color * m.color;
}

color3 TransparencyShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& srcrmi = param.rmi;
    
    const SceneObject& obj = srcrmi.rt->object;
    const Material& m = obj.material;
    
    return (renderer.tracePath(ThicknessRay(srcrmi.hit, param.inray.dir), (void*)&param)) * m.transparency;
}

color3 AnisotropicShader::shade(BSDFParam& param) {
    const RayRenderer& renderer = param.renderer;
    const auto& rmi = param.rmi;
    
    const SceneObject& obj = rmi.rt->object;
    const Material& m = obj.material;
    
    const vec3& normal = param.hi.normal;
    
    const vec3 dir = randomRayInHemisphere(normal);
    const Ray ray = ThicknessRay(rmi.hit, dir);
    
    color3 color = renderer.tracePath(ray, (void*)&param)
    + renderer.traceLight(rmi, param.hi);
    
    if (renderer.settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(param.hi.uv * m.texTiling).rgb;
        }
    }
    
    return color;
}

color3 MixShader::shade(BSDFParam& param) {
    const auto& srcrmi = param.rmi;
    
    const SceneObject& obj = srcrmi.rt->object;
    const Material& m = obj.material;
    
    color3 color;
    
    const float diffuse = 1.0f - m.glossy - m.refraction;
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
    
    float r = randomValue();
    if (r < diffuse) {
        param.enableLightSample = true;  // NEEを有効化
        color = diffuseShader.shade(param) / diffuse;
    } else if (r < diffuse + m.glossy) {
        param.enableLightSample = false; // NEEを無効化
        color = glossyShader.shade(param) / m.glossy;
    } else {
        param.enableLightSample = false; // NEEを無効化
        color = refractionShader.shade(param) / m.refraction;
    }
    
    return color;
}

}
