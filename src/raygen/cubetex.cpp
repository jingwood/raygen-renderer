///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "cubetex.h"

#define FORMAT_TAG_RMAP 0x70616d72

namespace raygen {

CubeTexture::CubeTexture() {
	for (int i = 0; i < 6; i++) {
		
	}
}

CubeTexture::~CubeTexture() {
}

void CubeTexture::createEmpty(const int width, const int height) {
	this->width = width;
	this->height = height;
	
	for (int i = 0; i < 6; i++) {
		Image* img = this->faces[i] = new Image(PixelDataFormat::PDF_RGB, 8);;
		img->createEmpty(width, height);
		this->faces[i] = img;
	}
}

void CubeTexture::clear() {
	for (int i = 0; i < 6; i++) {
		Image* img = this->faces[i];
		img->clear();
	}
}

Image** CubeTexture::getFaceImages() {
	return this->faces;
}

Image& CubeTexture::getFaceImage(const CubeTextureFace face) {
	return *this->faces[face];
}

void CubeTexture::paveFaces(Image& img) const {
	img.createEmpty(this->width * 4, this->height * 3);
	
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Left], 0, 0, img, 0, this->width);
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Forward], 0, 0, img, this->width, this->height);
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Right], 0, 0, img, this->width * 2, this->height);
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Back], 0, 0, img, this->width * 3, this->height);
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Top], 0, 0, img, this->width, 0);
	Image::copyRect(*this->faces[CubeTextureFace::CTF_Bottom], 0, 0, img, this->width, this->height * 2);
}

uint CubeTexture::getFaceRawDataLength() const {
	return this->width * this->height * 3;
}

uint CubeTexture::getRawDataLength() const {
	return this->getFaceRawDataLength() * 6;
}

void CubeTexture::getRawData(byte *buffer) const {
	const uint faceDataLen = this->getFaceRawDataLength();

	Image faceImageByte(PixelDataFormat::PDF_RGB, 8);
	faceImageByte.createEmpty(this->width, this->height);

	for (int i = 0; i < 6; i++) {
		const Image* faceImage = this->faces[i];
		Image::copyRect(*faceImage, faceImageByte);		
		memcpy(buffer + faceDataLen * i, faceImageByte.getBuffer(), faceDataLen);
	}
}

void CubeTexture::saveRawData(const string& path) const {
	FileStream fs(path);
	fs.openWrite();
	this->saveRawData(fs);
}

void CubeTexture::saveRawData(Stream& stream) const {
	const int dataLength = this->getRawDataLength();
	
	byte* buffer = new byte[dataLength];
	this->getRawData(buffer);
	
	CubeTexDataBlock dataBlock;
	dataBlock.formatTag = FORMAT_TAG_RMAP;
	dataBlock.len = sizeof(CubeTexDataBlock);
	dataBlock.ver = 0x0100;
	dataBlock.flags = 0;
	dataBlock.resX = this->width;
	dataBlock.resY = this->height;
	dataBlock.bounds = this->bbox;
	
	stream.write(&dataBlock, dataBlock.len);
	stream.write(buffer, dataLength);
	
	delete[] buffer;
	buffer = NULL;
}

}