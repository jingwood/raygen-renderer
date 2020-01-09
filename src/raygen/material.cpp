///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "material.h"

namespace raygen {

bool Material::equals(const Material &m2) const {
	if (this->color != m2.color
			|| this->emission != m2.emission
			|| this->glossy != m2.glossy
			|| this->spotRange != m2.spotRange
			|| this->roughness != m2.roughness
			|| this->transparency != m2.transparency
			|| this->refraction != m2.refraction
			|| this->refractionRatio != m2.refractionRatio) {
		return false;
	}
	
	if (this->name != m2.name) {
		return false;
	}
	
	if (this->texture != NULL || m2.texture != NULL) {
		if (this->texture	!= m2.texture) {
			return false;
		}
	}
	
	return true;
}

}