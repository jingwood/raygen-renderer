///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __ray_renderer_h__
#define __ray_renderer_h__

#include <stdio.h>
#include <map>

#include "raycommon.h"
#include "bsdf.h"
#include "renderer.h"
#include "cubetex.h"
#include "ucm/stopwatch.h"
#include "ugm/image.h"
#include "ugm/spacetree.h"

#if !defined(USE_SPACETREE)
#include "ugm/kdtree.h"
#endif /* USE_SPACETREE */

#define DEFAULT_RENDER_WIDTH 800
#define DEFAULT_RENDER_HEIGHT 600

#if defined(_WIN32) || defined(__APPLE__)

#if defined(DEBUG) || defined(_DEBUG) /* DEBUG */
#define ANTIALIAS_KERNEL_SIZE 1
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 1
#define DOF_SAMPLES 1
#define RENDER_THREADS 7
#else /* RELEASE */
#define ANTIALIAS_KERNEL_SIZE 3
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 20
#define DOF_SAMPLES 5
#define RENDER_THREADS 7
#endif /* END OF DEBUG */

#else /* DEBUG */
#define ANTIALIAS_KERNEL_SIZE 3
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 100
#define RENDER_THREADS 1
#define DOF_SAMPLES 0

#endif /* DEBUG */

#define RAY_MAX_DISTANCE 100.0f

namespace raygen {

class RayShaderProvider;

typedef SpaceTreeNode<const RayRenderTriangle*> RaySpaceTreeNode;
typedef SpaceTree<const RayRenderTriangle*> RaySpaceTree;

#ifndef USE_SPACETREE
typedef KDNode<const RayRenderTriangle*> RaySpaceKDTree;
#endif

typedef std::vector<const RayRenderTriangle*> RayRenderTriangleList;
typedef void (*MeshBakedCallback(const SceneObject& obj, const Image& img))();

struct LightSource {
	const SceneObject* object = NULL;
	
	vec3 transformedLocation;
	vec3 transformedNormal;
	
	LightSource() { }
};

class RayTransformedMesh {
public:
	const Mesh* mesh = NULL;
	BoundingBox bbox;
	RayRenderTriangleList triangleList;
	RaySpaceTree triangleTree;
};

typedef void RenderThreadCallback(float progressRate);

struct RendererSettings {
	int resolutionWidth = DEFAULT_RENDER_WIDTH, resolutionHeight = DEFAULT_RENDER_HEIGHT;
	int threads = RENDER_THREADS;
	int samples = TRACE_PATH_SAMPLES;
	int dofSamples = DOF_SAMPLES;
	byte shaderProvider = 5;
	byte antialiasKernelSize = ANTIALIAS_KERNEL_SIZE;

	bool enableAntialias = true;
	bool enablePointLightAntialias = true;
	bool enableColorSampling = true;
	bool enableRenderingPostProcess = true;
	bool enableBakingPostProcess = true;

	color3 worldColor = color3(1.0f, 0.95f, 0.9f) * 0.8f;
	color4 backColor = color4(1.0f, 0.95f, 0.9f, 0.0f) * 0.1f;
};

struct RenderThreadContext {
	float aspectRate;
	sizef renderSize;
	sizef halfRenderSize;
	sizef viewportSize;
	float viewScaleX, viewScaleY;
	float depthOfField;
	float depthOfFieldScale;
	float aperture;
	float halfAperture;
};

class RayRenderer : public Renderer {
private:
	int totalSampled = 0;
	vec3 cameraWorldPos;

	float* antialiasKernel = NULL;
//	int antialiasKernelSize = 3;

	std::vector<const RayTransformedMesh*> transformedMeshes;
	std::vector<LightSource> areaLightSources;
	std::vector<LightSource> pointLightSources;
	
	void initRenderThreadContext(RenderThreadContext* ctx);
  void renderThread(const RenderThreadContext& ctx, const int threadId);
	void renderAsyncThread(RenderThreadCallback* callback);
	
	void scanBoundingBoxNearestTriangle(const Ray& ray, const RayRenderTriangle* hitrt, RayMeshIntersection& rmi) const;
	void scanBoundingBoxSpaceTreeNearestTriangle(const Ray& ray, RayMeshIntersection& rmi) const;
	float scanBoundingBoxRayBlocked(const Ray& ray, const float maxt, const RayRenderTriangle* hitrt) const;

  bool putTriangleIntoChildrenNode(RaySpaceTreeNode* node, const RayRenderTriangle* rt);
  bool putTriangleIntoTree(RaySpaceTreeNode* node, const RayRenderTriangle* rt);
	void scanSpaceTreeNearestTriangle(const RaySpaceTreeNode* node, const Ray& ray, RayMeshIntersection& rmi) const;
  float scanSpaceTreeRayBlocked(const RaySpaceTreeNode* node, const Ray& ray, const float maxt, float* t_out = NULL) const;
	float scanBoundingBoxSpaceTreeRayBlocked(const Ray& ray, const float maxt, float* t_out = NULL) const;
	void scanSpaceTreeBoundingBox(const RaySpaceTreeNode* node, const Ray& ray,
																const RayRenderTriangle* hitrt, RayMeshIntersection& rmi) const;
	void calcHitInterpolation(const RayRenderTriangle& rt, const vec3& hit, HitInterpolation* hi) const;

	color3 traceAreaLight(const LightSource& lightSource, const RayMeshIntersection& rmi, const HitInterpolation& srchi) const;
	color3 tracePointLight(const LightSource& lightSource, const RayMeshIntersection& rmi, const HitInterpolation& srchi) const;

	color4 renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y) const;
	color4 traceRay(const Ray& ray) const;

protected:
	RaySpaceTree tree;
#ifndef USE_SPACETREE
	RaySpaceKDTree kdtree;
#endif
	
	std::vector<const RayRenderTriangle*> triangleList;
	Image4f renderingImage;
	float progressRate = 0.0f;

	void transformScene();
	void transformObject(SceneTransformStack& transformStack, SceneObject& obj);
	void clearTransformedScene();
	
	std::map<const Mesh*, RayRenderTriangleList> meshTriangles;
	
public:
	RendererSettings settings;
	RayShaderProvider* shaderProvider = NULL;
	std::function<void(float)> progressCallback = NULL;

	RayRenderer(const RendererSettings* settings = NULL);
	~RayRenderer();

//	color3 worldColor;
//	color3 backColor;
//	int threads = 1;
//	int samples = 10;
//	bool enableRenderColor = true;
//	bool enableRenderingPostProcess = false;
//	bool enableBakingPostProcess = true;

  void init();
  void render();
	
	color3 tracePath(const Ray& ray, void* shaderParam) const;
	color3 traceLight(const RayMeshIntersection& rmi, const HitInterpolation& srchi, const int samples = 1) const;
	color3 traceAllLight(const RayMeshIntersection& rmi, const HitInterpolation& srchi) const;

  float calcAO(const vec3& vertex, const vec3& normal, const float traceDistance = RAY_MAX_DISTANCE) const;
	float calcVertexAO(const Mesh& mesh, const int triangleIndex, const int vertexIndex, const float traceDistance);
	void calcVertexColors(Mesh& mesh);
	
  inline void setRenderSize(const sizei& size) {
		this->setRenderSize(size.width, size.height);
  }
	
	inline void setRenderSize(const int width, const int height) {
		this->renderingImage.createEmpty(width, height);
	}
  
  inline const Image& getRenderResult() const {
    return this->renderingImage;
  }
	
	void clearRenderResult();
  
//  inline const RaySpaceTree& getTree() const {
//    return this->tree;
//  }
};

class RayShaderProvider
{
public:
	RayRenderer* renderer = NULL;

	RayShaderProvider(RayRenderer* renderer = NULL) : renderer(renderer) { }
	virtual ~RayShaderProvider() { }
	virtual color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam = NULL) = 0;
};

class RaySimpleShaderProvider : public RayShaderProvider
{
	const vec3 lightSrc;

public:
	RaySimpleShaderProvider(RayRenderer* renderer)
		: RayShaderProvider(renderer)
		, lightSrc(normalize(vec3(1, 1, 1)))
	{ }
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam = NULL) {
		const float n = 0.1f + fmaxf(dot(lightSrc, hi.normal), 0);
    return color3(n, n, n);
  }
};

class RayAmbientOcclusionShaderProvider : public RayShaderProvider
{
public:
	float traceDistance = RAY_MAX_DISTANCE;

	RayAmbientOcclusionShaderProvider(RayRenderer* renderer)
		: RayShaderProvider(renderer)
	{ }

  color3 shade(const RayMeshIntersection& rmi, const Ray& inray, const HitInterpolation& hi, void* shaderParam = NULL) {
    const float c = clamp(this->renderer->calcAO(rmi.hit, hi.normal, traceDistance), 0.0f, 1.0f);
    return color3(c, c, c);
  }
};

class RayBSDFShaderProvider : public RayShaderProvider
{
private:
	MixShader mixShader;
	DiffuseShader diffuseShader;
	GlossyShader glossyShader;
	EmissionShader emissionShader;
	RefractionShader refractionShader;
	TransparencyShader transparencyShader;
	AnisotropicShader anisotropicShader;
	
public:
	RayBSDFShaderProvider(RayRenderer* renderer = NULL)
	: RayShaderProvider(renderer)	{
	}
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray,
							 const HitInterpolation& hi, void* shaderParam = NULL);
};

class RayBSDFBakeShaderProvider : public RayShaderProvider
{
	DiffuseShader diffuseShader;
	TransparencyShader transparencyShader;
	
public:
	RayBSDFBakeShaderProvider(RayRenderer* renderer = NULL)
	: RayShaderProvider(renderer) {
	}
	
	color3 shade(const RayMeshIntersection& rmi, const Ray& inray,
							 const HitInterpolation& hi, void* shaderParam = NULL);
};

}

#endif /* __ray_renderer_h__ */
