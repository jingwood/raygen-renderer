///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef material_h
#define material_h

#include <stdio.h>

#include "ucm/string.h"
#include "ugm/color.h"
#include "texture.h"

namespace raygen {

class Material
{
private:
  
public:
	string name;

	color3 color = color3(0.8f, 0.8f, 0.8f);
	
    float diffuse = 0.8f;
	float glossy = 0.0f;
	float roughness = 0.5f;
	float transparency = 0.0f;
	float refraction = 0.0f;
	float refractionRatio = 1.45f;
	// Wavelength-dependent IOR offset. 0 = no dispersion, ~0.02 = subtle
	// glass rainbow edge, ~0.05 = prism/crystal. Applied stochastically in
	// RefractionShader: one of R/G/B is picked per sample and its IOR is
	// scaled by (1 ± chromaDispersion).
	float chromaDispersion = 0.0f;

	float emission = 0.0f;
	float spotRange = 0.0f;
	
	string texturePath;
	vec2 texTiling = vec2::one;

	string normalmapPath;
	float normalMipmap = 0.0f;

	Texture* texture = NULL;
	Texture* normalmap = NULL;
	
	bool isLoaded = false;

	Material() { }
	
	bool equals(const Material& m2) const;
	inline bool operator ==(const Material& m2) const { return this->equals(m2); }
	inline bool operator !=(const Material& m2) const { return !this->equals(m2); }
};

}

#endif /* material_h */
