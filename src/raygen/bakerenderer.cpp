///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "bakerenderer.h"

#include <thread>
#include "ucm/ansi.h"
#include "ugm/imgfilter.h"

using namespace ucm;
using namespace ugm;

namespace raygen {

BakeRenderer::~BakeRenderer() {
	if (this->imgbits != NULL) {
		delete[] this->imgbits;
	}
}

void BakeRenderer::prepareBake() {
	if (this->scene == NULL) return;
	
	this->clearTransformedScene();
	this->transformScene();
	
	if (this->imgbits != NULL) {
		delete[] this->imgbits;
	}
	
	const int imgbitLen = this->renderingImage.width() * this->renderingImage.height();
	this->imgbits = new byte[imgbitLen];
	memset(this->imgbits, 0, imgbitLen);
	
	// build polygon tree
	std::vector<Triangle2D> triangles;
	
	for (int i = 0; i < this->triangleList.size(); i++) {
		Triangle2D t;
		t.v1 = this->triangleList[i]->uv4;
		t.v2 = this->triangleList[i]->uv5;
		t.v3 = this->triangleList[i]->uv6;
		t.bbox = BBox2D::fromTriangle(t.v1, t.v2, t.v3);
		t.bbox.inflate(vec2(this->margin, this->margin));
		triangles.push_back(t);
	}
	
	tree.build(triangles.data(), triangles.size());
}

void BakeRenderer::clearRenderResult() {
	this->renderingImage.fillRect(0, 0, this->renderingImage.width(), this->renderingImage.height(), colors::white);
}

void BakeRenderer::bakeMesh(const Mesh& mesh) {
//	this->renderingImage.clear();
	this->progressRate = 0;

	std::vector<std::thread> threads;

	for (int i = 0; i < this->settings.threads; i++) {
		threads.push_back(std::thread([this, &mesh, i] { this->bakeMeshThread2(mesh, i); }));
	}

	for (std::thread &th : threads) {
		th.join();
	}

//	if (this->enableBakingPostProcess) {
//		img::blur(this->renderingImage);
//	}

	/////////////////////////
//	RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];
//
//	for (auto rt : triangleList) {
//		this->pixelScanTriangle(*rt);
//	}
	
	////////////////////////
}

void BakeRenderer::bakeMesh3(const Mesh& mesh) {
	this->progressRate = 0;

	const RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];
	
	for (const auto* rt : triangleList) {
		if (rt->uv4 == vec2::zero && rt->uv5 == vec2::zero && rt->uv6 == vec2::zero) {
			continue;
		}
		
		this->fillVertex(*rt, rt->uv4);
		this->fillVertex(*rt, rt->uv5);
		this->fillVertex(*rt, rt->uv6);
	}
	
	///////////////////////////////////////////////////////////////////////
	
	std::vector<std::thread> threads;
	
	for (int i = 0; i < this->settings.threads; i++) {
		threads.push_back(std::thread([this, &mesh, i] { this->bakeMeshThread3(mesh, i); }));
	}
	
	for (std::thread &th : threads) {
		th.join();
	}
	
	///////////////////////////////////////////////////////////////////////
	
	Image& image = this->renderingImage;

	const int renderWidth = image.width();
	const int renderHeight = image.height();

//	const RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];

	for (const auto* rt : triangleList) {
		if (rt->uv4 == vec2::zero && rt->uv5 == vec2::zero && rt->uv6 == vec2::zero) {
			continue;
		}
//		this->fillVertex(*rt, rt->uv4);
//		this->fillVertex(*rt, rt->uv5);
//		this->fillVertex(*rt, rt->uv6);

		BBox2D box = rt->uvt2Info.box;
		box *= vec2(renderWidth, renderHeight);
		box.inflate(vec2(this->margin, this->margin));
		
		const int startx = floor(box.min.x);
		const int endx = ceil(box.max.x);
		const int starty = floor(box.min.y);
		const int endy = ceil(box.max.y);
		
		for (int x = startx; x <= endx; x++) {
			if (x < 0 || x >= renderWidth) continue;
			
			bool firstInTriangle = false;
			bool firstOutTriangle = false;
			
			color3 c;
			
			for (int y = starty; y <= endy; y++) {
				
				if (y < 0 || y >= renderHeight) continue;
				
				vec2 uv(((float)x + 0.5) / renderWidth, ((float)y + 0.5) / renderHeight);
				if (rt->uvt2Info.box.contains(uv) && rt->containsUVPoint(uv)) {
					
					if (imgbits[x + y * renderWidth] == 1) {
						c = image.getPixel(x, y);
					}
					
					if (!firstInTriangle) {
						firstInTriangle = true;
						
						for (int fy = y - 1; fy >= y - this->margin && fy >= starty; fy--) {
							if (fy < 0 || fy >= renderHeight) continue;
							
							const int bitIndex = x + fy * renderWidth;
							if (imgbits[bitIndex] == 0) {
								image.setPixel(x, fy, c);
								imgbits[bitIndex] = 1;
							}
						}
					}
				} else if (!firstOutTriangle && firstInTriangle) {
					firstOutTriangle = true;
					
					for (int fy = y; fy < y + this->margin && fy <= endy; fy++) {
						if (fy < 0 || fy >= renderHeight) continue;
						
						const int bitIndex = x + fy * renderWidth;
						if (imgbits[bitIndex] == 0) {
							image.setPixel(x, fy, c);
							imgbits[bitIndex] = 1;
						}
					}
				}
			}
		}
	}
}

void BakeRenderer::bakeMeshThread(const Mesh& mesh, const int threadId) {
	Image& image = this->renderingImage;

	const int renderWidth = image.width();
	const int renderHeight = image.height();
	
	const RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];
	
	vec2 uv;
	
	for (int y = threadId; y < renderHeight; y += this->settings.threads) {
		uv.v = ((float)y) / renderHeight + 0.00001f;
		
		for (int x = 0; x < renderWidth; x++) {
			uv.u = ((float)x) / renderWidth + 0.00001f;
			
			for (int i = 0; i < triangleList.size();i++) {
				const auto* rt = triangleList[i];

				if (rt->uv4 == vec2::zero && rt->uv5 == vec2::zero && rt->uv6 == vec2::zero) {
					//					printf("!");
				}
				else if (rt->containsUVPoint(uv)) {
					const color3 c = this->bakeMeshFragment(*rt, uv);
					image.setPixel(x, y, c);
				}
			}
		}
		
		if (y % 100 == 0) {
			printf(".");
		}
	}
	
}

void BakeRenderer::bakeMeshThread2(const Mesh& mesh, const int threadId) {
	Image& image = this->renderingImage;
	
	const int renderWidth = image.width();
	const int renderHeight = image.height();
	
	RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];

	for (int i = threadId; i < triangleList.size(); i += this->settings.threads) {
		auto* rt = triangleList[i];
		
		BBox2D box = rt->uvt2Info.box;
		
		for (int y = floor(box.min.y * renderHeight) - 1; y <= ceil(box.max.y * renderHeight) + 1; y++) {
			
			if (y < 0 || y >= renderHeight) continue;
			
			for (int x = floor(box.min.x * renderWidth) - 1; x <= ceil(box.max.x * renderWidth) + 1; x++) {

				if (x < 0 || x >= renderWidth) continue;

					vec2 uv(((float)x + 0.5) / renderWidth, ((float)y + 0.5) / renderHeight);
					if (rt->containsUVPoint(uv)) {
						const color3 c = this->bakeMeshFragment(*rt, uv + vec2(0.00001f, 0.00001f));
//						color3 c = colors::white;
						image.setPixel(x, y, c);
					}
			}
			
			if (i % 100 == 0) {
				int p = (int)(i * 100 / triangleList.size());
				if (p > progressRate) {
					progressRate = p;
					printf(ANSI_CLN_LEFT "%d%% ", p);
				}
			}
		}
	}
}

void BakeRenderer::bakeMeshThread3(const Mesh& mesh, const int threadId) {
	Image& image = this->renderingImage;
	
	const int renderWidth = image.width();
	const int renderHeight = image.height();
	
	RayRenderTriangleList& triangleList = this->meshTriangles[&mesh];
	
	for (int i = threadId; i < triangleList.size(); i += this->settings.threads) {
		auto* rt = triangleList[i];
		
		BBox2D box = rt->uvt2Info.box;
		box *= vec2(renderWidth, renderHeight);
		box.inflate(vec2(this->margin, this->margin));
	
		const int startx = floor(box.min.x) - 1;
		const int endx = ceil(box.max.x) + 1;
		const int starty = floor(box.min.y) - 1;
		const int endy = ceil(box.max.y) + 1;
		
		for (int y = starty - this->margin; y <= endy; y++) {
			
			if (y < 0 || y >= renderHeight) continue;

			bool firstInTriangle = false;
			bool firstOutTriangle = false;

			color3 c;

			for (int x = startx; x <= endx; x++) {
				
				if (x < 0 || x >= renderWidth) continue;
				
				vec2 uv(((float)x + 0.5) / renderWidth, ((float)y + 0.5) / renderHeight);
				if (rt->uvt2Info.box.contains(uv) && rt->containsUVPoint(uv)) {
					
					c = this->bakeMeshFragment(*rt, uv);
//					c = colors::white;
					image.setPixel(x, y, c);
					imgbits[x + y * renderWidth] = 1;
					
					if (!firstInTriangle) {
						firstInTriangle = true;
						
						for (int fx = x - 1; fx >= x - this->margin && fx >= startx; fx--) {
							if (fx < 0 || fx >= renderWidth) continue;

							vec2 uv2(((float)fx + 0.5) / renderWidth, ((float)y + 0.5) / renderHeight);
							
							if (!this->tree.hitAny(uv2)) {
								image.setPixel(fx, y, c);
								imgbits[fx + y * renderWidth] = 1;
							}
						}
					}
				} else if (!firstOutTriangle && firstInTriangle) {
					firstOutTriangle = true;
					
					for (int fx = x	; fx < x + this->margin && fx <= endx; fx++) {
						if (fx < 0 || fx >= renderWidth) continue;

						vec2 uv2(((float)fx + 0.5) / renderWidth, ((float)y + 0.5) / renderHeight);
						
						if (!this->tree.hitAny(uv2)) {
							image.setPixel(fx, y, c);
							imgbits[fx + y * renderWidth] = 1;
						}
					}
				}
				
			}
			
			if (progressCallback != NULL) {
				if (i % 100 == 0) {
					int p = (int)(i * 100 / triangleList.size());
					if (p > progressRate) {
						progressRate = p;
						progressCallback(progressRate);
//						printf(ANSI_RESET_LINE "%d%%", p);
//						fflush(stdout);
					}
				}
			}
		}

	}
}

void BakeRenderer::fillVertex(const RayRenderTriangle& rt, const vec2& v) {
	const int imgw = this->renderingImage.width();
	const int imgh = this->renderingImage.height();
	
	vec2 vp = v * vec2(imgw, imgh);
	BBox2D box(vp - vec2(this->margin, this->margin), vp + vec2(this->margin, this->margin));
	
//	const color3 c = colors::white;
	color3 c;
	bool first = true;
	
	for (int y = box.min.y; y <= box.max.y; y++) {
		for (int x = box.min.x; x <= box.max.x; x++) {
			if (x < 0 || y < 0 || x >= imgw || y >= imgh) {
				continue;
			}
			
			const int bitIndex = x + y * imgw;
			if (imgbits[bitIndex] == 0) {
				
				if (first) {
					c = this->bakeMeshFragment(rt, v);
//					c = colors::white;
					first = false;
				}
				
				this->renderingImage.setPixel(x, y, c);
				imgbits[bitIndex] = 1;
			}
		}
	}
}

color3 BakeRenderer::bakeMeshFragment(const RayRenderTriangle& rt, const vec2& uv) {
	color3 c;
	
	if (this->settings.enableAntialias) {
		constexpr float offset = 0.001;
		
		c += bakePoint(rt, uv);
		c += bakePoint(rt, uv + vec2(0, offset));
		c += bakePoint(rt, uv + vec2(offset, 0));
		c += bakePoint(rt, uv + vec2(offset, offset));
		
		return c * 0.25;
	} else {
		c = bakePoint(rt, uv);
	}
	
	return c;
}

color3 BakeRenderer::bakePoint(const RayRenderTriangle& rt, const vec2& uv) {
	const vec2 f1 = rt.uv4 - uv;
	const vec2 f2 = rt.uv5 - uv;
	const vec2 f3 = rt.uv6 - uv;
	
	const float a = 1.0f / cross(rt.uv4 - rt.uv5, rt.uv4 - rt.uv6);
	const float a1 = cross(f2, f3) * a;
	const float a2 = cross(f3, f1) * a;
	const float a3 = cross(f1, f2) * a;
	
	const vec3 p = rt.v1 * a1 + rt.v2 * a2 + rt.v3 * a3;
	RayMeshIntersection rmi(&rt, 0, p);
	
	HitInterpolation hi;
	hi.uv = uv;
	hi.normal = rt.n1 * a1 + rt.n2 * a2 + rt.n3 * a3;
	
	return this->shaderProvider->shade(rmi, ThicknessRay(p, -hi.normal), hi);
}

void BakeRenderer::bakeCubeTexture(CubeTexture& cubetex, const vec3& cameraLocation) {
	
	Camera* previousCamera = this->scene->mainCamera;
	
	Camera camera;
	camera.location = cameraLocation;
	camera.fieldOfView = 180.0f;
	
	this->scene->mainCamera = &camera;
//	float progressRate = 0;
//
//	auto& oldfun = this->progressCallback;
//
//	this->progressCallback = [](const float p) {
//		if (oldfun != NULL) {
//			oldfun(p / 600);
//		}
//	};
	
	for (int i = 0; i < 6; i++) {
		
		switch (i) {
			case CubeTextureFace::CTF_Left: camera.angle = vec3(0, 90, 0); break;
			case CubeTextureFace::CTF_Right: camera.angle = vec3(0, -90, 0); break;
			case CubeTextureFace::CTF_Top: camera.angle = vec3(90, 0, 0); break;
			case CubeTextureFace::CTF_Bottom: camera.angle = vec3(-90, 0, 0); break;
			case CubeTextureFace::CTF_Forward: camera.angle = vec3::zero; break;
			case CubeTextureFace::CTF_Back: camera.angle = vec3(0, 180, 0); break;
		}
		
		this->render();
		
		Image& faceImage = cubetex.getFaceImage((CubeTextureFace)i);
		Image::copy(this->renderingImage, faceImage);
		
		img::flipImageHorizontally(faceImage);
//		img::gamma(faceImage, 2.0f);
//		img::blur(faceImage);
	}
	
	this->scene->mainCamera = previousCamera;
}

}