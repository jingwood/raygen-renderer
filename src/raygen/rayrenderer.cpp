///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "rayrenderer.h"

#include <stdio.h>
#include <iostream>
#include <thread>

#include "ugm/functions.h"
#include "ugm/imgfilter.h"
#include "ucm/ansi.h"
#include "lambert.h"
#include "polygons.h"

#define CUT_OFF_BACK_TRACE

#define SPACE_TREE_DIMENSION 10
#define SPACE_TREE_NODE_ELEMENTS 3
#define SPACE_TREE_MAX_DEPTH 2

//#define USE_KDTREE
#define USE_SPACETREE

#ifdef USE_SPACETREE
#define USE_BOUNDING_BOX
#define USE_SPACE_TREE_IN_BOUNDING_BOX
#else
#define USE_KDTREE
#endif

#define AO_SAMPLES 50
#define AO_MAX_DISTANCE 10
#define AO_RANDOM_HEMISPHERE_RAY

#define TRACE_LIGHT_TRIES 1
#define TRACE_PATH_TRIES 1
#define TRACE_MAX_DEPTH 6

#define PP_GLOW_SIZE 0.1
#define PP_GLOW_GAMMA 0.9f
#define PP_GLOW_KERNEL 11

namespace raygen {

RayRenderer::RayRenderer(const RendererSettings* settings) {
	
	if (settings != NULL) {
		this->settings = *settings;
	}

	switch (this->settings.shaderProvider) {
		case 0:
			this->shaderProvider = new RaySimpleShaderProvider(this);
			break;
		case 1:
			this->shaderProvider = new RayAmbientOcclusionShaderProvider(this);
			break;
		case 2:
			this->shaderProvider = new LambertShaderProvider(this);
			break;
		case 3:
			this->shaderProvider = new LambertWithAOShaderProvider(this);
			//	this->shaderProvider = new LambertWithAOLightShaderProvider(this);
			break;
		case 5:
			this->shaderProvider = new RayBSDFShaderProvider(this);
			//	this->shaderProvider = new RayBSDFBakeShaderProvider(this);
			break;
	}
	
	this->renderingImage.setPixelDataFormat(PixelDataFormat::PDF_RGBA, 32);
	this->setRenderSize(this->settings.resolutionWidth, this->settings.resolutionHeight);

	/* initialize random seed */
	srand((unsigned int)time(NULL));
	
	if (this->settings.enableAntialias) {
		this->antialiasKernel = new float[this->settings.antialiasKernelSize * this->settings.antialiasKernelSize];
		gaussianDistributionGenKernel(this->antialiasKernel, this->settings.antialiasKernelSize, 5.0f);
	} else {
		this->antialiasKernel = NULL;
	}
}

RayRenderer::~RayRenderer() {
	if (this->shaderProvider != NULL) {
		delete this->shaderProvider;
		this->shaderProvider = NULL;
	}
	
	this->clearTransformedScene();
	
	if (this->antialiasKernel != NULL) {
		delete this->antialiasKernel;
		this->antialiasKernel = NULL;
	}
}

void RayRenderer::initRenderThreadContext(RenderThreadContext* ctx) {
	const Camera* camera = this->scene->mainCamera;
	if (camera == NULL) camera = &this->defaultCamera;
	
	const sizei& renderingImageSize = this->renderingImage.getSize();
	ctx->renderSize = sizef((float)renderingImageSize.width, (float)renderingImageSize.height);
	ctx->halfRenderSize = sizef(ctx->renderSize.width * 0.5f, ctx->renderSize.height * 0.5f);
	
	ctx->aspectRate = ctx->renderSize.width / ctx->renderSize.height;
	const float length = fabsf(camera->viewFar - camera->viewNear);
	const float viewportWidth = length * atanf(ANGLE_TO_DEGREE(camera->fieldOfView * 0.5f)) * 2.0;
	const float viewportHeight = viewportWidth / ctx->aspectRate;
	
	ctx->viewportSize = sizef(viewportWidth, viewportHeight);
	
	ctx->viewScaleX = ctx->viewportSize.width / ctx->renderSize.width;
	ctx->viewScaleY = ctx->viewportSize.height / ctx->renderSize.height;
	
	ctx->depthOfField = camera->depthOfField;
	ctx->depthOfFieldScale = (camera->depthOfField / length);
	ctx->aperture = 1.0f / camera->aperture;
	ctx->halfAperture = ctx->aperture * 0.5f;
}

void RayRenderer::clearRenderResult() {
	this->renderingImage.clear();
}

void RayRenderer::clearTransformedScene() {
	for (const auto& p : this->meshTriangles) {
		for (const auto* rt : p.second) {
			delete rt;
		}
	}
	
	this->meshTriangles.clear();
	
	for (auto tmesh : this->transformedMeshes) {
		delete tmesh;
	}
	
	this->transformedMeshes.clear();
	this->areaLightSources.clear();
	this->pointLightSources.clear();
}

void RayRenderer::transformScene() {
	if (this->scene == NULL) return;
	
	//  this->tree.initSpace(SPACE_TREE_DIMENSION, SPACE_TREE_MAX_DEPTH);

	this->triangleList.clear();

	for (SceneObject* obj : this->scene->getObjects()) {
		if (obj->visible) {
			this->transformObject(*this->transformStack, *obj);
		}
	}
	
#ifdef USE_KDTREE
	this->kdtree.reset();
	this->kdtree.build(this->triangleList.data(), this->triangleList.size());
#endif /* USE_SPACETREE */

	//	int count = 0;
	//	for (const auto& m : this->meshTriangles) {
	//		count += m.second.size();
	//	}
	//	printf("polygons: %d\n", count);
}

bool isSharedEdgeUV2(const Mesh& mesh, uint currentTid, const vec2& refv1, const vec2& refv2) {
	
	for (ulong k = 0; k < mesh.getTriangleCount(); k++) {
		if (currentTid == k) continue;
		
		vec2 fpv1, fpv2, fpv3;
		
		if (mesh.uvCount > 1) {
			mesh.getUV(1, k, &fpv1, &fpv2, &fpv3);
		}
		
		if (Edge::almostSame(refv1, refv2, fpv2, fpv1)
			|| Edge::almostSame(refv1, refv2, fpv3, fpv1)
			|| Edge::almostSame(refv1, refv2, fpv3, fpv2)) {
			return true;
		}
	}
	
	return false;
}

void RayRenderer::transformObject(SceneTransformStack& transformStack, SceneObject& obj) {
	transformStack.pushObject(obj);
	
	const Material& m = obj.material;
	
	const auto& meshes = obj.getMeshes();
	
	BoundingBox bbox;
	bool first = true;
	
	const Matrix4& viewModelMatrix = this->viewMatrix * this->transformStack->modelMatrix;

	Matrix4 normalMatrix = viewModelMatrix;
	normalMatrix.inverse();
	normalMatrix.transpose();
	
	if (obj.renderable && meshes.size() > 0) {
//		int count = 0;
		
		for (const Mesh* mesh : obj.getMeshes()) {
			auto& triangleList = this->meshTriangles[mesh];
			
			RayTransformedMesh* tmesh = new RayTransformedMesh();
			tmesh->mesh = mesh;
			this->transformedMeshes.push_back(tmesh);
			
			for (uint k = 0; k < mesh->getTriangleCount(); k++) {
				vec3 v1, v2, v3, n1, n2, n3;
				vec2 uv1, uv2, uv3, uv4, uv5, uv6;
				
				mesh->getVertex(k, &v1, &v2, &v3);
				mesh->getNormal(k, &n1, &n2, &n3);
				
				if (mesh->uvCount > 0) {
					mesh->getUV(0, k, &uv1, &uv2, &uv3);
				}
				if (mesh->uvCount > 1) {
					mesh->getUV(1, k, &uv4, &uv5, &uv6);
				}
				
				v1 = (vec4(v1, 1.0f) * viewModelMatrix).xyz;
				v2 = (vec4(v2, 1.0f) * viewModelMatrix).xyz;
				v3 = (vec4(v3, 1.0f) * viewModelMatrix).xyz;
				
				n1 = (vec4(n1, 0.0f) * normalMatrix).xyz.normalize();
				n2 = (vec4(n2, 0.0f) * normalMatrix).xyz.normalize();
				n3 = (vec4(n3, 0.0f) * normalMatrix).xyz.normalize();
				
				RayRenderTriangle* rt = new RayRenderTriangle(v1, v2, v3,
															  n1, n2, n3,
															  uv1, uv2, uv3,
															  uv4, uv5, uv6,
															  obj, *mesh);
				
//				rt->uvt2Info.shared.e1 = isSharedEdgeUV2(*mesh, k, uv5, uv4);
//				rt->uvt2Info.shared.e2 = isSharedEdgeUV2(*mesh, k, uv6, uv4);
//				rt->uvt2Info.shared.e3 = isSharedEdgeUV2(*mesh, k, uv6, uv5);
				
				if (first) {
					bbox.initTo(v1);
					bbox.expandTo(v2);
					bbox.expandTo(v3);
					
					first = false;
				}
				else {
					bbox.expandTo(v1);
					bbox.expandTo(v2);
					bbox.expandTo(v3);
				}
				
				triangleList.push_back(rt);

				this->triangleList.push_back(rt);
				
				tmesh->triangleList.push_back(rt);
			}
			
			bbox.finalize();
			tmesh->bbox = bbox;
			
			const int level = (int)log10(tmesh->triangleList.size() - 1);
			
			if (level > 0) {
				tmesh->triangleTree.initSpace(tmesh->bbox, level);
			}
			
			for (const auto rt : triangleList) {
				this->putTriangleIntoTree(&tmesh->triangleTree.root, rt);
			}
		}
	}
	
	bbox.finalize();
	obj.worldBbox = bbox;
	
	if (m.emission > 0) {
		
		LightSource ls;
		ls.object = &obj;
		
		if (obj.getMeshes().size() > 0) {
			this->areaLightSources.push_back(ls);
		} else {
			const float s1 = sinf(ANGLE_TO_DEGREE(obj.angle.x));
			const float c1 = cosf(ANGLE_TO_DEGREE(obj.angle.x));
			const float s2 = sinf(ANGLE_TO_DEGREE(obj.angle.y));
			const float c2 = cosf(ANGLE_TO_DEGREE(obj.angle.y));
			
			vec3 v = (vec4(0.0f, 0.0f, 0.0f, 1.0f) * viewModelMatrix).xyz;
			vec3 n = (vec4(normalize(vec3(c2 * s1, c2 * c1, s2)), 0.0f) * normalMatrix).xyz;
			
			ls.transformedLocation = v;
			ls.transformedNormal = n;
			
			this->pointLightSources.push_back(ls);
		}
	}
	
	for (SceneObject* child : obj.getObjects()) {
		if (child->visible) {
			this->transformObject(transformStack, *child);
		}
	}
	
	transformStack.popObject();
}

void RayRenderer::render() {
	if (this->shaderProvider == NULL) return;
	
	this->resetTransformMatrices();
	
	if (this->scene == NULL || this->scene->mainCamera == NULL) return;

	RenderThreadContext ctx;
	this->initRenderThreadContext(&ctx);
	
	Scene& scene = *this->scene;
	Camera& camera = *scene.mainCamera;
	const SceneObject* focusOnObj = NULL;

	if (scene.mainCamera != NULL) {
		
		if (!camera.focusOnObjectName.isEmpty()) {
			focusOnObj = scene.findObjectByName(camera.focusOnObjectName);
		
			if (focusOnObj) {
				
				BoundingBox bbox = focusOnObj->getBoundingBox();
				const float size = fmaxf(bbox.size.x, fmaxf(bbox.size.y, bbox.size.z));
				const vec3 ray = camera.getWorldLocation() - bbox.origin;
				const vec3 dir = ray.normalize();
				
				const float distance = size * 0.5 + size * 0.5f / tanf(camera.fieldOfView * 0.5f * M_PI / 180.f);

				camera.location = bbox.origin + dir * distance;
				camera.lookAt(bbox.origin, vec3::up);
			}
		}
		
		this->applyCameraTransform(camera);
	}
	
	this->cameraWorldPos = camera.getWorldLocation();

	this->clearTransformedScene();
	this->transformScene();
	
	this->progressRate = 0;

	std::vector<std::thread> threads;
	
	for (int i = 0; i < this->settings.threads; i++) {
		threads.push_back(std::thread([this, ctx, i] { this->renderThread(ctx, i); }));
	}
	
	for (std::thread &th : threads) {
		th.join();
	}
	
	if (this->settings.enableRenderingPostProcess) {
		Image glowimg(this->renderingImage.getPixelDataFormat(), 32);
		Image::copy(this->renderingImage, glowimg);
		glowimg.resize((int)((float)this->renderingImage.width() * PP_GLOW_SIZE),
			(int)((float)this->renderingImage.height() * PP_GLOW_SIZE));
		img::gamma(glowimg, PP_GLOW_GAMMA);
		img::gaussBlur(glowimg, PP_GLOW_KERNEL);
		glowimg.resize(this->renderingImage.getSize());
		img::calc(this->renderingImage, glowimg, img::CalcMethods::Lighter, 0.35f);
	}
}

void RayRenderer::renderAsyncThread(RenderThreadCallback* callback) {
	
}

void RayRenderer::renderThread(const RenderThreadContext& ctx, const int threadId) {
	
	const Camera* camera = this->scene->mainCamera;
	if (camera == NULL) camera = &this->defaultCamera;
	
	const float renderWidth = ctx.renderSize.width;
	const float renderHeight = ctx.renderSize.height;

	constexpr int pixelBlock = PIXEL_BLOCK;
	
	Ray ray(vec3(0.0001f, 0.0001f, camera->viewNear), vec3(0.0001f, 0.0001f, -camera->viewFar));
	
	for (int y = threadId * pixelBlock; y < renderHeight; y += pixelBlock * this->settings.threads) {
		for (int x = 0; x < renderWidth; x += pixelBlock) {
			const color4f c = this->renderPixel(ctx, ray, x, y);

#if PIXEL_BLOCK == 1
			this->renderingImage.setPixel(x, y, c);
#else
			this->renderingImage.fillRect(recti(x, y, pixelBlock, pixelBlock), c);
#endif /* PIXEL_BLOCK */
		}
		
		const float pr = (float)y / renderHeight;
		
		if (pr > this->progressRate) {
			this->progressRate = pr;

#if defined(DEBUG) || defined(DEBUG_LOCAL)
			const int percent = int(pr * 100.0f);
			if (percent % 10 == 0) {
				printf(ANSI_RESET_LINE "%d%%", percent);
				fflush(stdout);
			}
#endif /* DEBUG || defined(DEBUG_LOCAL) */

			if (this->progressCallback != NULL) {
				this->progressCallback(this->progressRate);
			}
		}
	}
}

color4f RayRenderer::renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y) const {
	
	vec3 F(0, 0, -ctx.depthOfField);
	color4f c = color4f(colors::black, this->settings.backColor.a);

	const bool antialiasAvailable = this->settings.enableAntialias && this->settings.antialiasKernelSize > 1;
	const int antialiasKernelSize = this->settings.enableAntialias ? this->settings.antialiasKernelSize : 1;
	const float halfAntialiasSize = (float)antialiasKernelSize * 0.5f;
	constexpr float aaoffset = 1.0f / ANTIALIAS_KERNEL_SIZE;
	const float dofSampleInv = 1.0f / (float)this->settings.dofSamples;

	for (int oy = 0; oy < antialiasKernelSize; oy++) {
		const float dy = -((float)y + ((float)oy - halfAntialiasSize) * aaoffset - ctx.halfRenderSize.height) * ctx.viewScaleY;
		
		for (int ox = 0; ox < antialiasKernelSize; ox++) {
			const float dx = ((float)x + ((float)ox - halfAntialiasSize) * aaoffset - ctx.halfRenderSize.width) * ctx.viewScaleX;

			color4f sampleColor;

			if (ctx.depthOfField >= 0.001f && this->settings.dofSamples > 0) {
				F.x = dx * ctx.depthOfFieldScale;
				F.y = dy * ctx.depthOfFieldScale;

				sampleColor = color3::zero;
				
				for (int i = 0; i < this->settings.dofSamples; i++) {
					ray.origin.x = randomValue() * ctx.aperture - ctx.halfAperture;
					ray.origin.y = randomValue() * ctx.aperture - ctx.halfAperture;
					ray.dir = (F - ray.origin).normalize();
					sampleColor += this->traceRay(ray);
				}
				
				sampleColor *= dofSampleInv;
			} else {
				ray.origin = vec3(randomValue() * 0.0001f, randomValue() * 0.0001f, 0);
				ray.dir = vec3(dx, dy, -50).normalize();
				sampleColor = this->traceRay(ray);
			}
			
			if (antialiasAvailable) {
				const float d = this->antialiasKernel[oy * this->settings.antialiasKernelSize + ox];
				c += sampleColor * d;
			} else {
				c += sampleColor;
			}
		}
	}
	
	return c;
}

void RayRenderer::emitPhotons() {
	for (const auto& lightSource : this->areaLightSources) {
		
		const SceneObject* obj = lightSource.object;
		if (obj != NULL) {

			const float emission = obj->material.emission;
			
			const auto& meshes = obj->getMeshes();
			if (meshes.size() > 0) {

				const Mesh* mesh = meshes[rand() % meshes.size()];

				const auto& triangleList = this->meshTriangles.at(mesh);
				if (triangleList.size() > 0) {

					const auto& triangle = *triangleList[rand() % triangleList.size()];

					Ray ray;
					ray.origin = randomPointInTriangle(triangle.tri);
					ray.dir = randomRayInHemisphere(triangle.faceNormal);
					
					this->emitPhoton(ray, emission);
				}
			}
		}
	}
}

void RayRenderer::emitPhoton(const Ray &ray, float photons) {
	RayMeshIntersection rmi(NULL, RAY_MAX_DISTANCE);
	this->findNearestTriangle(ray, rmi);
	
	if (rmi.rt != NULL) {
		if (rmi.rt->object.visible) {
			
		}
	}
}

color4 RayRenderer::traceRay(const Ray& ray) const {

	RayMeshIntersection rmi(NULL, 9999999.0f);
	this->findNearestTriangle(ray, rmi);

	if (rmi.rt != NULL) {
		VertexInterpolation hi;
		this->calcVertexInterpolation(*rmi.rt, rmi.hit, &hi);

		if (rmi.rt->object.visible) {
//			if (rmi.rt->object.material.emission <= 0 && dot(-ray.dir, hi.normal) < 0) {
//				return color3::zero;
//			}

			return clamp(this->shaderProvider->shade(rmi, ray, hi), 0.0f, 1.0f);
		}
	}

	return this->settings.backColor;
}

color3 RayRenderer::tracePath(const Ray& ray, void* shaderParam) const {
	for (int i = 0; i < TRACE_PATH_TRIES; i++) {
		RayMeshIntersection rmi(NULL, RAY_MAX_DISTANCE);
		this->findNearestTriangle(ray, rmi);

		if (rmi.rt != NULL) {
			VertexInterpolation hi;
			this->calcVertexInterpolation(*rmi.rt, rmi.hit, &hi);

			return this->shaderProvider->shade(rmi, ray, hi, shaderParam);
		}
	}

	return this->settings.backColor;
}

void RayRenderer::findNearestTriangle(const Ray& ray, RayMeshIntersection& rmi) const {

	#ifndef USE_KDTREE
	{
		#ifdef USE_BOUNDING_BOX
			this->scanBoundingBoxSpaceTreeNearestTriangle(ray, rmi);
		#else
			this->scanSpaceTreeNearestTriangle(&this->tree.root, ray, &srcrt, rmi);
		#endif // USE_BOUNDING_BOX
	}
	#elif defined(USE_KDTREE_MESH)
	{
		for (auto& tmesh : this->transformedMeshes) {
			if (rayIntersectBox(ray, tmesh->bbox)) {
				tmesh->kdtree.iterate(ray, [&ray, &srcrt, &rmi](const RayRenderTriangle* rt) {
					if (&srcrt == rt) return true;

					float t;
					vec3 hit;
					if (rt->intersectsRay(ray, rmi.t, t, hit)) {
						rmi = RayMeshIntersection(rt, t, hit);
					}

					return true;
				});
			}
		}
	}
	#else
	{
		this->kdtree.iterate(ray, [&ray, &rmi](const RayRenderTriangle* rt) {
			// if (&srcrt == rt) return true;

			float t;
			vec3 hit;
			if (rt->intersectsRay(ray, rmi.t, t, hit)) {
				rmi = RayMeshIntersection(rt, t, hit);
			}

			return true;
		});
	}
	#endif /* USE_KDTREE */
}

color3 RayRenderer::traceAreaLight(const LightSource& lightSource, const RayMeshIntersection& rmi, const VertexInterpolation& srchi) const {
	const SceneObject* obj = lightSource.object;
	if (obj == NULL) return color3::zero;

	const auto& meshes = obj->getMeshes();
	if (meshes.size() <= 0) return color3::zero;

	const Mesh* mesh = meshes[rand() % meshes.size()];

	const auto& triangleList = this->meshTriangles.at(mesh);
	if (triangleList.size() <= 0) return colors::transparent;

	const auto& triangle = *triangleList[rand() % triangleList.size()];

	const vec3 p = randomPointInTriangle(triangle.tri);
	const vec3 lightRay = p - rmi.hit;
	const vec3 lightNormal = normalize(lightRay);

	const float dotObjectToLight = dot(lightNormal, srchi.normal);

	constexpr float maxt = 0.99999f;
	
	if (dotObjectToLight > 0) {
		VertexInterpolation lightHit;
		calcVertexInterpolation(triangle, p, &lightHit);

		Ray ray = ThicknessRay(rmi.hit, lightRay);

#ifdef USE_SPACETREE

#if !defined(USE_BOUNDING_BOX)
		const float block = scanSpaceTreeRayBlocked(&this->tree.root, ray, maxt);
#else
		const float block = scanBoundingBoxSpaceTreeRayBlocked(ray, maxt);
#endif /* USE_BOUNDING_BOX */

#elif defined(USE_KDTREE_MESH)
		float block = 0.0;

		for (auto& tmesh : this->transformedMeshes) {
			if (rayIntersectBox(ray, tmesh->bbox)) {
				if (!tmesh->kdtree.iterate(ray, [&ray, &srcrmi](const RayRenderTriangle* rt) {
					if (srcrmi.rt == rt) return true;

					float t;
					vec3 hit;
					if (rt->intersectsRay(ray, maxt, t, hit)) {
						return false;
					}

					return true;
				})) {
					block = 1.0;
					break;
				}
			}
		}

#elif defined(USE_KDTREE)

		const float block = this->kdtree.iterate(ray, [&ray](const RayRenderTriangle* rt) {

			float t;
			vec3 hit;
			if (rt->intersectsRay(ray, maxt, t, hit)) {
				return false;
			}

			return true;
		}) ? 0.0f : 1.0f;

#endif /* USE_KDTREE */

		if (block < 1.0f) {
			const auto& lightMat = lightSource.object->material;
			const float dist = powf(lightRay.length(), -2.0f);
			
			return lightMat.color * (lightMat.emission * dist * dotObjectToLight * fabs(dot(-lightNormal, lightHit.normal)));
		}
	}

	return color3::zero;
}

color3 RayRenderer::tracePointLight(const LightSource& lightSource, const RayMeshIntersection& rmi, const VertexInterpolation& srchi) const {
	const vec3 lightray = lightSource.transformedLocation - rmi.hit;

	Ray ray = ThicknessRay(rmi.hit, lightray);
	constexpr float maxt = 0.99999f;

#ifndef USE_KDTREE

#if !defined(USE_BOUNDING_BOX)
	const float block = scanSpaceTreeRayBlocked(&this->tree.root, ray, maxt);
#else
	const float block = scanBoundingBoxSpaceTreeRayBlocked(ray, maxt);
#endif /* USE_BOUNDING_BOX */

#else

	const float block = this->kdtree.iterate(ray, [&ray](const RayRenderTriangle* rt) {
		float t;
		vec3 hit_unused;
		if (rt->intersectsRay(ray, maxt, t, hit_unused)) {
			if ((rt->object.material.transparency < 0.01f || rt->object.material.refraction > 0.1f))
				return false;
		}

		return true;
	}) ? 0.0f : 1.0f;

#endif /* USE_KDTREE */
	
	const SceneObject* light = lightSource.object;

	if (block < 1.0f) {
		const vec3 lightrayNormal = lightray.normalize();

		float dotToObject = dot(lightrayNormal, srchi.normal);
		float dotToLight = dot(lightrayNormal, lightSource.transformedNormal);

		if (dotToObject > 0) {
			const Material& lightMat = light->material;

			if (lightMat.spotRange > 0) {
				// spot light
				const float spotRangeDot = cosf(ANGLE_TO_DEGREE(lightMat.spotRange * 0.5f));
				dotToLight = dotToLight * smoothstep(fmaxf(spotRangeDot - 0.1f, 0.0f), fminf(spotRangeDot + 0.1f, 1.0f), dotToLight);
			}
			else {
				dotToLight = fabsf(dotToObject);
			}

			if (dotToLight > 0) {
				// distance attenuation
				const float da = powf(lightray.length(), -2.0f);

				// calc the lum from this light
				const float lum = lightMat.emission * dotToLight * da;

				// calc the phong specluar
				float specluar = 0;
				
				const float glossy = rmi.rt->object.material.glossy;
				
				if (glossy > 0) {
					if (this->settings.shaderProvider < 5) {
						const vec3 r = reflect(-lightray, srchi.normal).normalize();
						const float d = dot(r, (cameraWorldPos - rmi.hit).normalize());
						if (d > 0) {
							specluar = powf(d, 10000 * glossy);
						}
					} else {
						specluar = 0;
					}
				}
				
				// final light color
				return clamp(lightMat.color * ((lum + specluar)));
			}
		}
	}

	return color3::zero;
}

color3 RayRenderer::traceLight(const RayMeshIntersection& rmi, const VertexInterpolation& srchi, const int samples) const {
	color3 areaLightColor, pointLightColor;

	const int areaLightSourceCount = (int)this->areaLightSources.size();
	const int pointLightSourceCount = (int)this->pointLightSources.size();

	if (areaLightSourceCount > 0) {
		for (int i = 0; i < samples; i++) {
			const LightSource& ls = this->areaLightSources[rand() % areaLightSourceCount];
			areaLightColor += this->traceAreaLight(ls, rmi, srchi);
		}

		areaLightColor /= (float)samples;
	}

	if (pointLightSourceCount > 0) {
		if (pointLightSourceCount == 1) {
			pointLightColor = this->tracePointLight(this->pointLightSources[0], rmi, srchi);
		}
		else {
			for (int i = 0; i < samples; i++) {
				const LightSource& ls = this->pointLightSources[rand() % pointLightSourceCount];
				pointLightColor += this->tracePointLight(ls, rmi, srchi);
			}
			pointLightColor /= (float)pointLightSourceCount;

//			for (const LightSource& ls : pointLightSources) {
//				pointLightColor += this->tracePointLight(ls, vertex, srchi);
//			}

		}
	}

	return areaLightColor + pointLightColor;
}

color3 RayRenderer::traceAllLight(const RayMeshIntersection& rmi, const VertexInterpolation& srchi) const {
	
	color3 areaLightColor, pointLightColor;

	const int areaLightSourceCount = (int)this->areaLightSources.size();

	if (areaLightSourceCount > 0) {
		const int samples = this->settings.samples;
		
		for (int i = 0; i < samples; i++) {

			const LightSource& ls = this->areaLightSources[rand() % areaLightSourceCount];
			areaLightColor += this->traceAreaLight(ls, rmi, srchi);
		}

		areaLightColor /= (float)samples;
	}

	for (const LightSource& ls : this->pointLightSources) {
		pointLightColor += this->tracePointLight(ls, rmi, srchi);
	}
	
	return areaLightColor + pointLightColor + this->settings.worldColor;
}

void RayRenderer::calcVertexInterpolation(const RayRenderTriangle& rt, const vec3& hit, VertexInterpolation* hi) const {
	const vec3 f1 = rt.v1 - hit;
	const vec3 f2 = rt.v2 - hit;
	const vec3 f3 = rt.v3 - hit;

//	const float a1 = fmaxf(cross(f2, f3).length() * rt.ti.a, 0);
//	const float a2 = fmaxf(cross(f3, f1).length() * rt.ti.a, 0);
//	const float a3 = fmaxf(cross(f1, f2).length() * rt.ti.a, 0);

	const float a1 = (cross(f2, f3).length() * rt.ti.a);
	const float a2 = (cross(f3, f1).length() * rt.ti.a);
	const float a3 = (cross(f1, f2).length() * rt.ti.a);

	hi->uv = rt.uv1 * a1 + rt.uv2 * a2 + rt.uv3 * a3;
	hi->normal = rt.n1 * a1 + rt.n2 * a2 + rt.n3 * a3;
}

#if !defined(AO_RANDOM_HEMISPHERE_RAY)
static vec3 generateHemisphereVectorByEulerAngles(const float a1, const float a2, const vec3& normal) {
	const float t2 = (2.0f * PI * a1);
	const float p2 = acosf(1.0f - 2.0f * a2);
	
	const float sp2 = sinf(a2);
	
	vec3 dir = vec3(sp2 * cosf(a1), sp2 * sinf(a1), cosf(a2)).normalize();
	
	if (dot(dir, normal) < 0) {
		dir = -dir;
	}
	
	return dir;
}
#endif /* AO_RANDOM_HEMISPHERE_RAY */

float RayRenderer::calcAO(const vec3& vertex, const vec3& normal, const float traceDistance) const {
	int s = 0;
	//float s = 0;

#if defined(AO_RANDOM_HEMISPHERE_RAY)

	for (int i = 0; i < this->settings.samples; i++) {
		
		const vec3& dir = randomRayInHemisphere(normal);
		Ray ray = ThicknessRay(vertex, dir);

#if defined(USE_SPACETREE)

#if !defined(USE_BOUNDING_BOX)
		const float block = this->scanSpaceTreeRayBlocked(&this->tree.root, ray, traceDistance);
#else
		const float block = this->scanBoundingBoxSpaceTreeRayBlocked(ray, traceDistance);
#endif /* USE_BOUNDING_BOX */

#elif defined(USE_KDTREE)

		const float block = this->kdtree.iterate(ray, [&ray, &traceDistance](const RayRenderTriangle* rt) {
			//if (rmi.rt == rt) return true;

			float t;
			vec3 hit;
			if (rt->intersectsRay(ray, traceDistance, t, hit)) {
				if ((rt->object.material.transparency < 0.01f || rt->object.material.refraction > 0.1f))
					return false;
			}

			return true;
		}) ? 0.0f : 1.0f;

#endif /* USE_SPACETREE */

		if (block < 1.0f) {
			s++;
		}
	}

#else /* NOT: AO_RANDOM_HEMISPHERE_RAY */
	
	const float stride = PI / this->samples;

	for (float a1 = 0; a1 < 1; a1 += stride) {

		const vec3& dir = generateHemisphereVectorByEulerAngles(a1, 1.0f - a1, hi.normal);

#if !defined(USE_BOUNDING_BOX)
		if (this->scanSpaceTreeRayBlocked(&this->tree.root, ThicknessRay(rmi.hit, dir), traceDistance))
#else
		if (this->scanBoundingBoxSpaceTreeRayBlocked(Ray(rmi.hit, dir), traceDistance) < 1.0f)
#endif /* USE_BOUNDING_BOX */
		{
			s++;
		}
	}
#endif /* AO_RANDOM_HEMISPHERE_RAY */
	
	return (float)s / this->settings.samples;
}

float RayRenderer::calcVertexAO(const Mesh& mesh, const int triangleIndex, const int vertexIndex, const float traceDistance) {
	auto& tr = this->meshTriangles[&mesh][triangleIndex];
	
	const vec3& v = tr->vs[vertexIndex];
	const vec3& n = tr->ns[vertexIndex];
	
	int s = 0;
	
	for (int i = 0; i < this->settings.samples; i++) {
		const vec3& dir = randomRayInHemisphere(n);
		
		Ray ray(v, dir);
		
#if defined(USE_SPACETREE)
		
#if !defined(USE_BOUNDING_BOX)
		const float block = this->scanSpaceTreeRayBlocked(&this->tree.root, ray, traceDistance);
#else
		const float block = this->scanBoundingBoxSpaceTreeRayBlocked(ray, traceDistance);
#endif /* USE_BOUNDING_BOX */
		
#elif defined(USE_KDTREE)
		const float block = this->kdtree.iterate(ray, [&ray, &traceDistance](const RayRenderTriangle* rt) {
			
			float t;
			vec3 hit;
			if (rt->intersectsRay(ray, traceDistance, t, hit)) {
				return false;
			}
			
			return true;
		}) ? 0.0f : 1.0f;
#endif /* USE_KDTREE */
		
		if (block < 1.0f) {
			s++;
		}
	}
	
	return (float)s / this->settings.samples;
}

void RayRenderer::calcVertexColors(Mesh &mesh) {
	
	// vertex AO
	mesh.createColorBuffer();
	
	const auto& triangleList = this->meshTriangles[&mesh];
	
	for (int ti = 0; ti < triangleList.size(); ti++) {
		color3 gray[3];
		
		RayMeshIntersection rmi;
		VertexInterpolation hi;
		
//		vec3 vs[3], ns[3];
		const auto* t = triangleList[ti];
		
		for (int vi = 0; vi < 3; vi++) {
			const vec3& vertex = t->vs[vi];
			hi.normal = t->ns[vi];
			rmi.hit = vertex;
			gray[vi] = color3(.1, .1, .1) + this->traceLight(rmi, hi) * 0.9;
//			if ( vi==0) gray[vi] = colors::red;
//			if ( vi==1) gray[vi] = colors::green;
//			if ( vi==2) gray[vi] = colors::blue;
		}
		
		mesh.setColor(ti, gray[0], gray[1], gray[2]);
	}
}

inline bool RayRenderer::putTriangleIntoChildrenNode(RaySpaceTreeNode* node, const RayRenderTriangle* rt) {
  bool inLeft = node->left->intersectTriangle(rt->tri);
  bool inRight = node->right->intersectTriangle(rt->tri);
  
  if (inLeft && inRight)
    return false;
  else if (inLeft)
    return putTriangleIntoTree(node->left, rt);
  else if (inRight)
    return putTriangleIntoTree(node->right, rt);
  else
    return false;
}

bool RayRenderer::putTriangleIntoTree(RaySpaceTreeNode* node, const RayRenderTriangle* rt) {
	if (!node->splitted
		|| !putTriangleIntoChildrenNode(node, rt))
	{
		node->list.push_back(rt);
	}

	return true;
}

inline bool rayIntersectTriangle3(const Ray& ray, const RayRenderTriangle& rt, const float maxt, float& t, vec3& hit) {
	const float dist = -dot(rt.ti.l, vec4(ray.origin, 1.0f)) / dot(rt.ti.l, vec4(ray.dir, 0.0f));
	
	if (dist < 0/* || isnan(dist)*/ || dist > maxt) {
		return false;
	}
	
	t = dist;
	hit = ray.origin + ray.dir * dist;
	
	vec3 c;
	
	c = cross(rt.v2 - rt.v1, hit - rt.v1);
	if (dot(rt.ti.pd, c) < 0) return false;
	
	c = cross(rt.v3 - rt.v2, hit - rt.v2);
	if (dot(rt.ti.pd, c) < 0)  return false;
	
	c = cross(rt.v1 - rt.v3, hit - rt.v3);
	if (dot(rt.ti.pd, c) < 0) return false;
	
	return true;
	//return pointInTriangle3D(hit, normalize(pc.normalizedpd), rt.tri);
}

#if !defined(USE_SPACE_TREE_IN_BOUNDING_BOX)
void RayRenderer::scanBoundingBoxNearestTriangle(const Ray& ray, const RayRenderTriangle* hitrt,
																								 RayMeshIntersection& rmi) const
{
	float t;
	vec3 hit;
	
	for (const auto tmesh : this->transformedMeshes) {
		if (rayIntersectBox(ray, tmesh->bbox)) {
			for (const auto rt : tmesh->triangleList) {
				
				if (rt == hitrt) {
					continue;
				}
				
				if (rt->intersectsRay(ray, rmi.t, t, hit)) {
					rmi = RayMeshIntersection(rt, t, hit);
				}
			}
		}
	}
}
#endif /* USE_SPACE_TREE_IN_BOUNDING_BOX */

#ifdef USE_SPACETREE

void RayRenderer::scanBoundingBoxSpaceTreeNearestTriangle(const Ray& ray, RayMeshIntersection& rmi) const {
	vec3 hit;
	
	for (const auto tmesh : this->transformedMeshes) {
		if (rayIntersectBox(ray, tmesh->bbox)) {
			this->scanSpaceTreeNearestTriangle(&tmesh->triangleTree.root, ray, rmi);
		}
	}
}

void RayRenderer::scanSpaceTreeNearestTriangle(const RaySpaceTreeNode* node,
																							 const Ray& ray, RayMeshIntersection& rmi) const
{
  // self
  for (const RayRenderTriangle* rt : node->list) {
		float t;
    vec3 hit;
    
    //if (rayIntersectTriangle3(ray, *rt, rmi.t, t, hit)) {
    //  rmi = RayMeshIntersection(rt, t, hit);
    //}
		if ( rt->intersectsRay(ray, rmi.t, t, hit)) {
			rmi = RayMeshIntersection(rt, t, hit);
		}
  }

  // children
  if (node->splitted) {
    if (node->left->intersectRay(ray)) {
			scanSpaceTreeNearestTriangle(node->left, ray, rmi);
    }
    
    if (node->right->intersectRay(ray)) {
			scanSpaceTreeNearestTriangle(node->right, ray, rmi);
    }
  }
}

#if !defined(USE_BOUNDING_BOX)
void RayRenderer::scanSpaceTreeBoundingBox(const RaySpaceTreeNode* node,
																							 const Ray& ray, const RayRenderTriangle* hitrt,
																							 RayMeshIntersection& rmi) const
{
	// self
	for (const RayRenderTriangle* rt : node->list) {
		if (rt == hitrt) {
			continue;
		}
		
		float t;
		vec3 hit;
		
		if (rayIntersectTriangle3(ray, *rt, rmi.t, t, hit)) {
			rmi = RayMeshIntersection(rt, t, hit);
		}
	}
	
	// children
	if (node->splitted) {
		if (node->left->intersectRay(ray)) {
			scanSpaceTreeNearestTriangle(node->left, ray, hitrt, rmi);
		}
		
		if (node->right->intersectRay(ray)) {
			scanSpaceTreeNearestTriangle(node->right, ray, hitrt, rmi);
		}
	}
}
#endif /* USE_BOUNDING_BOX */

#if !defined(USE_BOUNDING_BOX)
float RayRenderer::scanBoundingBoxRayBlocked(const Ray& ray, const float maxt, const RayRenderTriangle* hitrt) const {
	float t;
	vec3 hit;
	
	for (const auto tmesh : this->transformedMeshes) {
		if (rayIntersectBox(ray, tmesh->bbox)) {
			for (const auto rt : tmesh->triangleList) {
				
				if (rt == hitrt) {
					continue;
				}
				
				if (rayIntersectTriangle3(ray, *rt, maxt, t, hit)) {
					return clamp(1.0f - rt->object.material.transparency, 0.0f, 1.0f);
				}
			}
		}
	}
	
	return 0.0f;
}
#endif /* USE_BOUNDING_BOX */

float RayRenderer::scanBoundingBoxSpaceTreeRayBlocked(const Ray& ray, const float maxt, float* t_out) const {
	vec3 hit;
	
	for (const auto tmesh : this->transformedMeshes) {
		if (rayIntersectBox(ray, tmesh->bbox)) {
			const float block = scanSpaceTreeRayBlocked(&tmesh->triangleTree.root, ray, maxt, t_out);
			
			if (block > 0) {
				return block;
			}
		}
	}
	
	return 0.0f;
}

float RayRenderer::scanSpaceTreeRayBlocked(const RaySpaceTreeNode* node, const Ray& ray, const float maxt, float* t_out) const
{
	for (const RayRenderTriangle* rt : node->list)
	{
		float t;
		vec3 hit_noused;

		if (rt->intersectsRay(ray, maxt, t, hit_noused)) {
			if (t_out != NULL) {
				*t_out = t;
			}
			
			if (rt->object.material.transparency < 0.01f || rt->object.material.refraction > 0.1f) {
				return 1.0f;
			}
			//return 1.0f - rt->object.material.transparency;
		}
	}

	if (node->splitted) {
		if (node->left->intersectRay(ray)) {
			const float block = scanSpaceTreeRayBlocked(node->left, ray, maxt);
			if (block > 0) return block;
		}
		
		if (node->right->intersectRay(ray)) {
			const float block = scanSpaceTreeRayBlocked(node->right, ray, maxt);
			if (block > 0) return block;
		}
	}

  return 0.0f;
}

#endif /* USE_SPACETREE */

color3 RayBSDFShaderProvider::shade(const RayMeshIntersection& rmi, const Ray& inray,
                                    const VertexInterpolation& hi, void* shaderParam) {
	const Material& m = rmi.rt->object.material;

	BSDFParam param(*this->renderer, rmi, inray, hi);

	if (m.emission > 0.0f) {
		return (normalize(m.color) + 1.0f);
//		return this->emissionShader.shade(param);
	}

#ifdef CUT_OFF_BACK_TRACE
	if (dot(inray.dir, hi.normal) > 0.0f) {
		if (m.transparency > 0.001f) {
			const BSDFParam* sp = (BSDFParam*)shaderParam;

			if (sp != NULL && sp->passes + 1 <= TRACE_MAX_DEPTH) {
				param.passes = sp->passes + 1;
				return transparencyShader.shade(param);
			} else {
				return color3::zero;
			}
		}
		else if (m.refraction < 0.001f && m.glossy > 0.001f) {
			
//			if (shaderParam != NULL) {
//				const BSDFParam* sp = (BSDFParam*)shaderParam;
//				
//				if (sp->passes + 1 >= TRACE_MAX_DEPTH) {
//					return color3::zero;
//				}
//			}
//		}
//		else {
			return color3::zero;
		}
	}
#endif /* CUT_OFF_BACK_TRACE */

	if (shaderParam != NULL) {
		const BSDFParam* sp = (BSDFParam*)shaderParam;

		if (sp->passes + 1 >= TRACE_MAX_DEPTH) {
			if (1.0f - m.glossy - m.refraction > 0.00001f) {
				const color3 light = this->renderer->traceLight(rmi, hi);

				color3 color;
				if (this->renderer->settings.enableColorSampling) {
					color = m.color;

					if (m.texture != NULL) {
						color *= m.texture->sample(param.hi.uv * m.texTiling).rgb;
					}
				}

				return light * color;
			} else {
				return color3::zero;
			}
		}

		if (m.transparency > 0.001f) {
			return transparencyShader.shade(param);
		}
		else {
			param.passes = sp->passes + 1;
			return mixShader.shade(param);
		}
	}
	else {
		
		color3 color;
		
		//int diffuse = 1.0f - m.glossy - m.refraction - m.transparency;
		
		const int samples = renderer->settings.samples;// (diffuse > 0.001f || m.roughness > 0.001f) ? renderer->samples : 1;
		
		for (int i = 0; i < samples; i++) {
			
			if (m.transparency > 0.01f) {
				color += mixShader.shade(param) * (1.0f - m.transparency) + transparencyShader.shade(param);
			}
			else {
				color += mixShader.shade(param);
			}
		}
		
		return color /= (float)samples;
//		color3 color;
//		
//			if (m.transparency > 0.01f) {
//				color = mixShader.shade(param) * (1.0f - m.transparency) + transparencyShader.shade(param);
//			}
//			else {
//				color = mixShader.shade(param);
//			}
//		}
//		
//		return color;
	}
}

color3 RayBSDFBakeShaderProvider::shade(const RayMeshIntersection& rmi, const Ray& inray,
																				const VertexInterpolation& hi, void* shaderParam) {
	const Material& m = rmi.rt->object.material;
	
	if (m.emission > 0.0f) {
		return (m.color * m.emission);
	}

	BSDFParam param(*this->renderer, rmi, inray, hi);

	if (m.transparency > 0.01f) {
		return this->transparencyShader.shade(param);
	}
	
	if (shaderParam != NULL) {

		BSDFParam* sp = (BSDFParam*)shaderParam;
		
		if (sp->passes >= 2) {
			return this->renderer->traceLight(rmi, hi) * m.color;
		}

		param.passes = sp->passes + 1;
	}
	
#ifdef CUT_OFF_BACK_TRACE
	if (m.transparency <= 0.001f && dot(-inray.dir, hi.normal) <= 0.0f) {
		return color3::zero;
	}
#endif /* CUT_OFF_BACK_TRACE */
	
	color3 color;
	
	int samples = this->renderer->settings.samples;
	
	if (param.passes > 0) samples = 1;
	
	for (int i = 0; i < samples; i++) {
		color += diffuseShader.shade(param);
	}
	
	return color / (float)samples;
}

}
