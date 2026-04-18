///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __RAY_SHADER_H__
#define __RAY_SHADER_H__

#include "ugm/color.h"
#include "raycommon.h"

namespace raygen {

class RayRenderer;

class BSDFParam
{
public:
	RayRenderer& renderer;
    const RayTriangleIntersectionInfo& interInfo;
    const Ray& inray;
	const VertexInterpolation& vi;
	int passes = 0;
    // Accumulated path throughput from the eye to this hit (before this hit's
    // own BSDF attenuation). Consumed by Russian Roulette in the shader
    // provider to decide whether to keep extending the path.
    color3 throughput = color3(1.0f, 1.0f, 1.0f);
    void* sourceShader = NULL;
    bool enableLightSample = false;

	BSDFParam(RayRenderer& renderer, const RayTriangleIntersectionInfo& interInfo,
              const Ray& inray, const VertexInterpolation& vi,
              int passes = 0, void* sourceShader = NULL)
	: renderer(renderer), interInfo(interInfo), inray(inray), vi(vi), passes(passes), sourceShader(sourceShader)
	{ }
};

class BSDFShader
{
public:
	virtual color3 shade(BSDFParam& param) = 0;
};

class DiffuseShader : public BSDFShader
{
public:
	color3 shade(BSDFParam& param);
};

class EmissionShader : public BSDFShader
{
public:
	color3 shade(BSDFParam& param);
};

class GlossyShader : public BSDFShader
{
public:
  color3 shade(BSDFParam& param);
};

class RefractionShader : public BSDFShader
{
public:
	color3 shade(BSDFParam& param);
};

class GlassShader : public BSDFShader
{
public:
	color3 shade(BSDFParam& param);
};

class TransparencyShader : public BSDFShader
{
public:
  color3 shade(BSDFParam& param);
};

class AnisotropicShader : public BSDFShader
{
public:
	color3 shade(BSDFParam& param);
};

class MixShader : public BSDFShader {
private:
	DiffuseShader diffuseShader;
	GlossyShader glossyShader;
	RefractionShader refractionShader;
	GlassShader glassShader;
	
public:
	color3 shade(BSDFParam& param);
};

}

#endif /* __RAY_SHADER_H__ */
