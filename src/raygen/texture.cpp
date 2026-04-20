///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "texture.h"
#include "ugm/imgcodec.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace raygen {

namespace {

// Minimal Radiance .hdr (RGBE) loader. Handles the common new-format RLE
// layout that every HDRI asset today uses; falls back to false on anything
// else (old uncompressed format, non-RGBE, bad header) so the caller can try
// another codec or fail cleanly.
bool loadRadianceHDR(Image& image, const string& path) {
    FILE* f = fopen(path.getBuffer(), "rb");
    if (f == NULL) return false;

    // Header: ASCII lines, ending with a blank line. Look for the RGBE format
    // tag and then the resolution line.
    char line[256];
    bool rgbeOK = false;
    bool gotMagic = false;
    while (fgets(line, (int)sizeof(line), f) != NULL) {
        if (line[0] == '\n' || line[0] == '\r') break;
        if (!gotMagic && (strncmp(line, "#?RADIANCE", 10) == 0 || strncmp(line, "#?RGBE", 6) == 0)) {
            gotMagic = true;
        }
        if (strncmp(line, "FORMAT=", 7) == 0) {
            if (strncmp(line + 7, "32-bit_rle_rgbe", 15) == 0) rgbeOK = true;
        }
    }
    if (!gotMagic || !rgbeOK) { fclose(f); return false; }

    int W = 0, H = 0;
    if (fscanf(f, "-Y %d +X %d\n", &H, &W) != 2 || W <= 0 || H <= 0) {
        fclose(f); return false;
    }

    image.setPixelDataFormat(PixelDataFormat::PDF_RGB, 32);
    image.createEmpty(W, H);

    std::vector<unsigned char> scanline((size_t)W * 4);

    for (int y = 0; y < H; y++) {
        unsigned char hdr[4];
        if (fread(hdr, 1, 4, f) != 4) { fclose(f); return false; }

        if (hdr[0] == 2 && hdr[1] == 2 && (hdr[2] & 0x80) == 0) {
            // New RLE: four channel streams, each RLE-compressed.
            const int len = (hdr[2] << 8) | hdr[3];
            if (len != W) { fclose(f); return false; }

            for (int chan = 0; chan < 4; chan++) {
                int p = 0;
                while (p < W) {
                    unsigned char count;
                    if (fread(&count, 1, 1, f) != 1) { fclose(f); return false; }
                    if (count > 128) {
                        const int runLen = count & 0x7f;
                        unsigned char val;
                        if (fread(&val, 1, 1, f) != 1) { fclose(f); return false; }
                        for (int i = 0; i < runLen; i++) scanline[(p + i) * 4 + chan] = val;
                        p += runLen;
                    } else {
                        const int n = count;
                        for (int i = 0; i < n; i++) {
                            unsigned char val;
                            if (fread(&val, 1, 1, f) != 1) { fclose(f); return false; }
                            scanline[(p + i) * 4 + chan] = val;
                        }
                        p += n;
                    }
                }
            }
        } else {
            // Old uncompressed format — refuse rather than silently misread.
            fclose(f); return false;
        }

        for (int x = 0; x < W; x++) {
            const unsigned char r = scanline[x * 4 + 0];
            const unsigned char g = scanline[x * 4 + 1];
            const unsigned char b = scanline[x * 4 + 2];
            const unsigned char e = scanline[x * 4 + 3];
            // RGBE decoding: channel * 2^(E - 128) / 256. When E is 0 the pixel
            // is black; ldexp handles that without branching.
            const float scale = (e == 0) ? 0.0f : ldexpf(1.0f, (int)e - 128 - 8);
            image.setPixel(x, y, color4f(r * scale, g * scale, b * scale, 1.0f));
        }
    }

    fclose(f);
    return true;
}

bool pathEndsWithHDR(const string& path) {
    const char* s = path.getBuffer();
    const int n = path.length();
    if (n < 4) return false;
    return (s[n-4] == '.' && (s[n-3] == 'h' || s[n-3] == 'H')
                          && (s[n-2] == 'd' || s[n-2] == 'D')
                          && (s[n-1] == 'r' || s[n-1] == 'R'));
}

}  // namespace

Texture::Texture()
: image(PixelDataFormat::PDF_RGBA) {
}

bool Texture::loadFromFile(const string& imagePath) {
    if (pathEndsWithHDR(imagePath)) {
        if (loadRadianceHDR(this->image, imagePath)) {
            this->isHDR = true;
            this->sRGB = false;
            return true;
        }
        printf("load hdr failed: %s\n", imagePath.getBuffer());
        return false;
    }

	try {
		loadImage(this->image, imagePath);

		return true;
	} catch (const Exception&) {
		printf("load image failed: %s\n", imagePath.getBuffer());
	}

	return false;
}

namespace {
// Exact sRGB → linear companding curve. The linear segment near zero avoids
// the singularity of a pure power curve; the upper segment is a 2.4-gamma
// with a tiny offset. Shader math is all linear, so every LDR texture sample
// needs this on its way in.
inline float srgbToLinear(float c) {
    if (c <= 0.04045f) return c * (1.0f / 12.92f);
    return powf((c + 0.055f) * (1.0f / 1.055f), 2.4f);
}
}

color4f Texture::sample(const vec2 &uv) const {
	const int x = modulo((int)(uv.u * this->image.width()), this->image.width());
	const int y = modulo((int)(uv.v * this->image.height()), this->image.height());

	color4f px = this->image.getPixel(x, y);
	if (this->sRGB) {
		px.r = srgbToLinear(px.r);
		px.g = srgbToLinear(px.g);
		px.b = srgbToLinear(px.b);
	}
	return px;
}

Texture* Texture::createFromFile(const string& path) {
	auto tex = new Texture();
	tex->loadFromFile(path);
	return tex;
}

}