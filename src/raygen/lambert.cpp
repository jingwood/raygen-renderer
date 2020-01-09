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

color3 LambertShaderProvider::shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam) {
	const Material& m = rmi.rt->object.material;
	
	if (m.emission > 0) {
		return m.color * m.emission;
	}
	
	color3 light;

	light = this->renderer->traceAllLight(rmi, hi) * (0.75f + m.roughness * 0.5f);
	
	color3f color = light;// + light * powf(fmaxf(dot(hi.normal, rmi.hit - ), 0.0), m.glossy);
	
	if (this->renderer->settings.enableColorSampling) {
		color *= m.color;
		
		if (m.texture != NULL) {
			color *= m.texture->sample(hi.uv * m.texTiling).rgb;
		}
	}
	
	if (m.transparency > 0.0f) {
		color = m.color * (light + 0.5f) * (1.0f - m.transparency)
			+ this->renderer->tracePath(ThicknessRay(rmi.hit, inray.dir), shaderParam) * m.transparency;
	}
	
	return color;
}

color3 LambertWithAOShaderProvider::shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam) {
	const Material& m = rmi.rt->object.material;
	
	if (m.emission > 0) {
		return m.color * m.emission;
	}
	
	const color3 light = this->renderer->traceAllLight(rmi, hi) * (0.75f + m.roughness * 0.5f);
	
	color3 color = light * 0.2 + light * 0.8 * renderer->calcAO(rmi.hit, hi.normal, 2.0);
	
	if (this->renderer->settings.enableColorSampling) {
		color *= m.color;
		
		if (m.texture != NULL) {
			color *= m.texture->sample(hi.uv * m.texTiling).rgb;
		}
	}
	
	if (m.transparency > 0.0f) {
		color += this->renderer->tracePath(ThicknessRay(rmi.hit, inray.dir), shaderParam) * m.transparency;
	}
	
	return color;
}

color3 LambertWithAOLightShaderProvider::shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam) {
	const Material& m = rmi.rt->object.material;
	
	if (m.emission > 0.0f) {
		return m.color * m.emission;
	}
	
	if (!this->renderer->settings.enableColorSampling && m.transparency > 0.0f) {
		return this->renderer->tracePath(ThicknessRay(rmi.hit, inray.dir), shaderParam);
	}
	
	const color3 light = this->renderer->traceAllLight(rmi, hi) * (0.75f + m.roughness * 0.5f);
	
	color3 color = (light + powf(renderer->calcAO(rmi.hit, hi.normal, 1.0f), 0.5f) * 0.5f);
	
	if (this->renderer->settings.enableColorSampling) {
		color *= m.color;
		
		if (m.texture != NULL) {
			color *= m.texture->sample(hi.uv * m.texTiling).rgb;
		}
	}
	
	if (m.transparency > 0.0f) {
		color = color * (1.0f - m.transparency)
			+ this->renderer->tracePath(ThicknessRay(rmi.hit, inray.dir), shaderParam) * m.transparency;
	}
	
	return color;
}

}