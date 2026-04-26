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

class HomogeneousMedium;
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
    // Solid-angle pdf of the outgoing direction the caller sampled via its
    // BSDF strategy. Used as the BSDF side of the power-heuristic MIS weight
    // at the next hit — both when it lands on an emitter (vs area-light NEE)
    // and when the ray escapes to the envmap (vs envmap NEE). BSDF-agnostic:
    // DiffuseShader stores cosθ/π, GlossyShader stores the GGX VNDF pdf, a
    // delta mirror leaves it at 0 which disables MIS (full contribution).
    float bsdfSampledPdf = 0.0f;
    // When the eye ray has committed to a single wavelength band for
    // chromatic dispersion, this records which channel (0/1/2 = R/G/B) the
    // whole path is tracing. -1 means the path hasn't hit a dispersive
    // material yet — the next RefractionShader hit will pick the channel
    // and propagate it downstream so every interface refracts consistently.
    int chromaChannel = -1;

    // Participating medium the ray is currently travelling through. NULL =
    // vacuum (no σ, no scattering — same as the legacy non-volumetric path).
    // Defaults to vacuum at construction; tracePath seeds it from
    // scene.globalMedium for the eye ray and RefractionShader swaps it on
    // entry/exit of an object's interiorMedium. Pointer-only — lifetime is
    // owned by the Scene / SceneObject.
    const HomogeneousMedium* currentMedium = NULL;

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
