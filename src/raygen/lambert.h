///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef lambert_h
#define lambert_h

#include <stdio.h>
#include "rayrenderer.h"

namespace raygen {

class LambertShaderProvider : public RayShaderProvider
{
private:
	float gaussKernel[25];
	
public:
	LambertShaderProvider(RayRenderer* renderer)
	: RayShaderProvider(renderer)
	{
		gaussianDistributionGenKernel(gaussKernel, 5);
	}
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const VertexInterpolation& hi, void* shaderParam = NULL);
};

class LambertWithAOShaderProvider : public RayShaderProvider
{
public:
	LambertWithAOShaderProvider(RayRenderer* renderer = NULL)
	: RayShaderProvider(renderer)
	{ }
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const VertexInterpolation& hi, void* shaderParam = NULL);
};

class LambertWithAOLightShaderProvider : public RayShaderProvider
{
public:
	LambertWithAOLightShaderProvider(RayRenderer* renderer = NULL)
	: RayShaderProvider(renderer)
	{ }
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const VertexInterpolation& hi, void* shaderParam = NULL);
};

}

#endif /* lambert_h */
