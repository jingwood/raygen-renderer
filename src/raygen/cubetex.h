///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef cubetex_h
#define cubetex_h

#include "ugm/types3d.h"
#include "ugm/image.h"
#include "ucm/file.h"

using namespace ugm;

namespace raygen {

enum CubeTextureFace : byte {
	CTF_Right = 0,
	CTF_Left = 1,
	CTF_Top = 2,
	CTF_Bottom = 3,
	CTF_Back = 4,
	CTF_Forward = 5,
};

class CubeTexture {
private:
	union {
		struct {
			Image* left = NULL;
			Image* right = NULL;
			Image* up = NULL;
			Image* down = NULL;
			Image* forward = NULL;
			Image* back = NULL;
		};
		Image* faces[6];
	};
	int width = 0, height = 0;
	
public:
	BoundingBox bbox;

	CubeTexture();
	~CubeTexture();
	
	void createEmpty(const int width, const int height);
	void clear();
	
	Image** getFaceImages();
	Image& getFaceImage(const CubeTextureFace face);
	
	void paveFaces(Image& img) const;
	
	uint getFaceRawDataLength() const;
	uint getRawDataLength() const;
	void getRawData(byte* buffer) const;
	void saveRawData(const string& path) const;
	void saveRawData(Stream& stream) const;
};

struct CubeTexDataBlock {
	uint formatTag;
	uint len;
	ushort ver;
	ushort flags;
	ushort resX;
	ushort resY;
	BoundingBox bounds;
};

}

#endif /* cubetex_h */
