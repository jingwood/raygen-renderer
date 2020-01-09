///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "texture.h"
#include "ugm/imgcodec.h"

namespace raygen {

Texture::Texture()
: image(PixelDataFormat::PDF_RGBA) {
}

bool Texture::loadFromFile(const string& imagePath) {

	try {
		loadImage(this->image, imagePath);
		
		return true;
	} catch (const Exception&) {
		printf("load image failed: %s\n", imagePath.getBuffer());
	}

	return false;
}

color4f Texture::sample(const vec2 &uv) const {
	const int x = modulo((int)(uv.u * this->image.width()), this->image.width());
	const int y = modulo((int)(uv.v * this->image.height()), this->image.height());
	
//	if (uv.u < 0 || uv.v < 0) {
//		printf("%f %f - %d %d - %d %d\n", uv.u, uv.v, x, y, this->image.width(), this->image.height());
//	}
	
	return this->image.getPixel(x, y);
}

Texture* Texture::createFromFile(const string& path) {
	auto tex = new Texture();
	tex->loadFromFile(path);
	return tex;
}

}