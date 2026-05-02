///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "texture.h"
#include "ugm/imgcodec.h"
#include "ucm/stream.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

namespace raygen {

namespace {

// Byte-stream readers. The HDR header is line-oriented ASCII; the raw scanline
// data is bytes. Two small adapters give us "is the next byte X?" / "read N
// bytes" / "read a line ending in \n" without committing the loader to FILE*
// (so we can decode an embedded archive chunk too).

struct ByteReader {
    virtual ~ByteReader() {}
    virtual bool readByte(unsigned char& out) = 0;
    virtual bool readBytes(unsigned char* out, size_t n) = 0;
};

struct FileByteReader : public ByteReader {
    FILE* f;
    explicit FileByteReader(FILE* f) : f(f) {}
    bool readByte(unsigned char& out) override {
        return std::fread(&out, 1, 1, f) == 1;
    }
    bool readBytes(unsigned char* out, size_t n) override {
        return std::fread(out, 1, n, f) == n;
    }
};

struct StreamByteReader : public ByteReader {
    ucm::Stream& s;
    explicit StreamByteReader(ucm::Stream& s) : s(s) {}
    bool readByte(unsigned char& out) override {
        return s.read(&out, 1) == 1;
    }
    bool readBytes(unsigned char* out, size_t n) override {
        return s.read(out, (uint)n) == n;
    }
};

// Pull one '\n'-terminated line into `out` (capacity `cap`). Returns the
// number of characters read (excluding the NUL); 0 on EOF or error. Strips
// nothing — caller compares prefixes via strncmp.
int readLine(ByteReader& r, char* out, int cap) {
    int n = 0;
    while (n < cap - 1) {
        unsigned char c;
        if (!r.readByte(c)) break;
        out[n++] = (char)c;
        if (c == '\n') break;
    }
    out[n] = '\0';
    return n;
}

// Core RGBE decoder shared by file and stream paths. Reads the header,
// resolution, then the RLE-compressed scanlines from `r`. Returns false on
// malformed / unsupported input (old non-RLE format, non-RGBE, EOF before
// data complete). Caller is expected to have positioned `r` at the start of
// the file.
bool decodeRadianceHDR(Image& image, ByteReader& r) {
    char line[256];
    bool rgbeOK = false;
    bool gotMagic = false;
    while (true) {
        const int len = readLine(r, line, (int)sizeof(line));
        if (len <= 0) return false;
        if (line[0] == '\n' || line[0] == '\r') break;
        if (!gotMagic && (strncmp(line, "#?RADIANCE", 10) == 0 || strncmp(line, "#?RGBE", 6) == 0)) {
            gotMagic = true;
        }
        if (strncmp(line, "FORMAT=", 7) == 0) {
            if (strncmp(line + 7, "32-bit_rle_rgbe", 15) == 0) rgbeOK = true;
        }
    }
    if (!gotMagic || !rgbeOK) return false;

    // Resolution line: "-Y H +X W\n". We need to read it byte-by-byte rather
    // than fscanf because the underlying reader doesn't necessarily support
    // formatted parsing.
    if (readLine(r, line, (int)sizeof(line)) <= 0) return false;
    int H = 0, W = 0;
    if (sscanf(line, "-Y %d +X %d", &H, &W) != 2 || W <= 0 || H <= 0) return false;

    image.setPixelDataFormat(PixelDataFormat::PDF_RGB, 32);
    image.createEmpty(W, H);

    std::vector<unsigned char> scanline((size_t)W * 4);

    for (int y = 0; y < H; y++) {
        unsigned char hdr[4];
        if (!r.readBytes(hdr, 4)) return false;

        if (hdr[0] == 2 && hdr[1] == 2 && (hdr[2] & 0x80) == 0) {
            const int len = (hdr[2] << 8) | hdr[3];
            if (len != W) return false;

            for (int chan = 0; chan < 4; chan++) {
                int p = 0;
                while (p < W) {
                    unsigned char count;
                    if (!r.readByte(count)) return false;
                    if (count > 128) {
                        const int runLen = count & 0x7f;
                        unsigned char val;
                        if (!r.readByte(val)) return false;
                        for (int i = 0; i < runLen; i++) scanline[(p + i) * 4 + chan] = val;
                        p += runLen;
                    } else {
                        const int n = count;
                        for (int i = 0; i < n; i++) {
                            unsigned char val;
                            if (!r.readByte(val)) return false;
                            scanline[(p + i) * 4 + chan] = val;
                        }
                        p += n;
                    }
                }
            }
        } else {
            // Old uncompressed format — refuse rather than silently misread.
            return false;
        }

        for (int x = 0; x < W; x++) {
            const unsigned char rB = scanline[x * 4 + 0];
            const unsigned char gB = scanline[x * 4 + 1];
            const unsigned char bB = scanline[x * 4 + 2];
            const unsigned char e  = scanline[x * 4 + 3];
            // RGBE decoding: channel * 2^(E - 128) / 256. When E is 0 the
            // pixel is black; ldexp handles that without branching.
            const float scale = (e == 0) ? 0.0f : ldexpf(1.0f, (int)e - 128 - 8);
            image.setPixel(x, y, color4f(rB * scale, gB * scale, bB * scale, 1.0f));
        }
    }

    return true;
}

bool loadRadianceHDR(Image& image, const string& path) {
    FILE* f = fopen(path.getBuffer(), "rb");
    if (f == NULL) return false;
    FileByteReader r(f);
    const bool ok = decodeRadianceHDR(image, r);
    fclose(f);
    return ok;
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
	const int W = (int)this->image.width();
	const int H = (int)this->image.height();
	if (W <= 0 || H <= 0) return color4f(0.0f, 0.0f, 0.0f, 0.0f);

	// Guard against NaN/huge UVs (e.g. meshes that arrive without texture
	// coordinates, or a material with a runaway texTiling). Float→int is UB
	// on NaN and saturates unpredictably past INT_MAX, and we can't let a
	// bogus index slip past the bounds check in Image::getPixel.
	if (!(uv.u == uv.u) || !(uv.v == uv.v)) return color4f(0.0f, 0.0f, 0.0f, 0.0f);

	// Fold UV into [0, 1) first, so the multiply by W/H stays in range.
	float fu = uv.u - floorf(uv.u);
	float fv = uv.v - floorf(uv.v);
	if (fu < 0.0f || fu >= 1.0f) fu = 0.0f;
	if (fv < 0.0f || fv >= 1.0f) fv = 0.0f;

	int x = (int)(fu * W);
	int y = (int)(fv * H);
	if (x >= W) x = W - 1;
	if (y >= H) y = H - 1;
	if (x < 0) x = 0;
	if (y < 0) y = 0;

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

bool Texture::loadHDRFromStream(ucm::Stream& stream) {
    StreamByteReader r(stream);
    if (decodeRadianceHDR(this->image, r)) {
        this->isHDR = true;
        this->sRGB  = false;
        return true;
    }
    return false;
}

}