///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "lambert.h"
#include "ugm/types3d.h"

namespace raygen {

color3 LambertShaderProvider::shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam) {
    const Material& m = interInfo.triangle->object.material;
    
    if (m.emission > 0) {
        return m.color * m.emission;
    }
        
    color3 directLight = this->renderer->lambertTraceLights(interInfo.hit, vi.normal) * 0.3;
    
    color3f color = directLight;
    
    if (this->renderer->settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(vi.uv * m.texTiling).rgb;
        }
    }
    
    if (m.transparency > 0.0f) {
        color = m.color * (directLight + 0.5f) * (1.0f - m.transparency)
        + this->renderer->tracePath(ThicknessRay(interInfo.hit, inray.dir), shaderParam) * m.transparency;
    }
    
    return color;
}

color3 LambertWithAOShaderProvider::shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& hi, void* shaderParam) {
    const Material& m = interInfo.triangle->object.material;
    
    if (m.emission > 0) {
        return m.color * m.emission;
    }
    
    const color3 light = this->renderer->lambertTraceLights(interInfo.hit, hi.normal) * 0.3;
    
    color3 color = light * 0.2 + light * 0.8 * renderer->calcAO(interInfo.hit, hi.normal, 2.0);
    
    if (this->renderer->settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(hi.uv * m.texTiling).rgb;
        }
    }
    
    if (m.transparency > 0.0f) {
        color += this->renderer->tracePath(ThicknessRay(interInfo.hit, inray.dir), shaderParam) * m.transparency;
    }
    
    return color;
}

color3 LambertWithAOLightShaderProvider::shade(const RayTriangleIntersectionInfo& interInfo,
                                               const Ray& inray, const VertexInterpolation& vi, void* shaderParam) {
    const Material& m = interInfo.triangle->object.material;
    
    if (m.emission > 0.0f) {
        return m.color * m.emission;
    }
    
    if (!this->renderer->settings.enableColorSampling && m.transparency > 0.0f) {
        return this->renderer->tracePath(ThicknessRay(interInfo.hit, inray.dir), shaderParam);
    }
    
    const color3 light = this->renderer->lambertTraceLights(interInfo.hit, vi.normal) * (0.75f + m.roughness * 0.5f);
    
    color3 color = (light + powf(renderer->calcAO(interInfo.hit, vi.normal, 1.0f), 0.5f) * 0.5f);
    
    if (this->renderer->settings.enableColorSampling) {
        color *= m.color;
        
        if (m.texture != NULL) {
            color *= m.texture->sample(vi.uv * m.texTiling).rgb;
        }
    }
    
    if (m.transparency > 0.0f) {
        color = color * (1.0f - m.transparency)
        + this->renderer->tracePath(ThicknessRay(interInfo.hit, inray.dir), shaderParam) * m.transparency;
    }
    
    return color;
}

}
