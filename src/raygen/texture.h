///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef texture_hpp
#define texture_hpp

#include <stdio.h>
#include "ucm/string.h"
#include "ugm/ugm.h"

namespace raygen {

class Texture
{
private:
	Image image;

public:
	Texture();

	// True once the file was decoded as HDR (Radiance .hdr) — caller uses this
	// to skip sRGB decode and to know the pixel values may exceed 1.0.
	bool isHDR = false;

	bool loadFromFile(const string& imagePath);

	const inline Image& getImage() const { return this->image; }
	inline Image& getImage() { return this->image; }

	color4f sample(const vec2& uv) const;

	static Texture* createFromFile(const string& path);
};

}

#endif /* texture_hpp */
