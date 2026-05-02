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
#include "ucm/stream.h"
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
	// LDR textures (JPG / PNG / …) are authored in sRGB space; we decode them
	// to linear at sample() time so the BSDF math stays linear and a "0.5 grey"
	// in the file actually reflects ~21% of incoming light. HDR textures are
	// already linear and bypass the decode.
	bool sRGB = true;

	bool loadFromFile(const string& imagePath);

	// Load a Radiance .hdr (RGBE) image directly from a Stream. Used by the
	// archive-based bundle loader so an embedded envmap chunk can be decoded
	// without writing a temp file first. Returns true on success and sets
	// isHDR/sRGB the same way loadFromFile does for the disk path.
	bool loadHDRFromStream(ucm::Stream& stream);

	const inline Image& getImage() const { return this->image; }
	inline Image& getImage() { return this->image; }

	color4f sample(const vec2& uv) const;

	static Texture* createFromFile(const string& path);
};

}

#endif /* texture_hpp */
