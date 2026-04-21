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
#include "bvh.h"
#include "renderer.h"
#include "cubetex.h"
#include "ucm/stopwatch.h"
#include "ucm/string.h"
#include "ugm/image.h"
#include "ugm/spacetree.h"

#define DEFAULT_RENDER_WIDTH 800
#define DEFAULT_RENDER_HEIGHT 600

#if defined(_WIN32) || defined(__APPLE__)

#if defined(DEBUG) || defined(_DEBUG) /* DEBUG */
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 2
#define RENDER_THREADS 7
#else /* RELEASE */
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 20
#define RENDER_THREADS 7
#endif /* END OF DEBUG */

#else /* DEBUG */
#define PIXEL_BLOCK 1
#define TRACE_PATH_SAMPLES 100
#define RENDER_THREADS 1

#endif /* DEBUG */

#define RAY_MAX_DISTANCE 100.0f

namespace raygen {

class RayShaderProvider;

typedef SpaceTreeNode<const RenderMeshTriangle*> RaySpaceTreeNode;
typedef SpaceTree<const RenderMeshTriangle*> RaySpaceTree;

typedef std::vector<const RenderMeshTriangle*> RayRenderTriangleList;
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
	byte shaderProvider = 5;

	bool enableAntialias = true;
	bool enablePointLightAntialias = true;
	bool enableColorSampling = true;
	bool enableRenderingPostProcess = false;
	bool enableBakingPostProcess = true;
	bool enableDenoise = false;
	bool cullBackFace = false;

	int denoiseLevels = 5;
	float denoiseSigmaColor = 0.4f;
	float denoiseSigmaNormal = 128.0f;
	float denoiseSigmaDepth = 0.1f;
	float denoiseIntensity = 1.0f;  // 0 = pass-through, 1 = full À-Trous

	float bloomThreshold = 0.7f;   // post-gamma luma at which bloom starts
	float bloomStrength = 0.35f;   // lighter-blend strength when compositing
	float bloomSizeAspect = 0.15f; // glow buffer size relative to main (downsample)

	// Non-empty path prefix enables dumping each post-process stage to
	// <prefix>-bloom-01-threshold.jpg etc. Main writes the scene base-name
	// here when --dump-bloom is passed.
	ucm::string postprocessDumpPath;

	color3 worldColor = color3(1.0f, 0.95f, 0.9f) * 0.1f;
	color4 backColor = color4(1.0f, 0.95f, 0.9f, 0.0f) * 0.2f;
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
	int apertureBlades = 0;
	float apertureRotation = 0.0f;  // radians
    float exposure = 1.0;
};

struct ViewRaySurfaceInfo {
    bool hitted = false;
//    color4 color;
    RayTriangleIntersectionInfo interInfo;
    VertexInterpolation hi;
    const Material* mat = NULL;
};

class RayRenderer : public Renderer {
private:
	int totalSampled = 0;
	vec3 cameraWorldPos;

	std::vector<const RayTransformedMesh*> transformedMeshes;
	std::vector<LightSource> areaLightSources;
	std::vector<LightSource> pointLightSources;
	
	void initRenderThreadContext(RenderThreadContext* ctx);
    void renderThread(const RenderThreadContext& ctx, const int threadId);
	void renderAsyncThread(RenderThreadCallback* callback);
	
	void findNearestTriangle(const Ray& ray, RayTriangleIntersectionInfo& info) const;
	void scanBoundingBoxNearestTriangle(const Ray& ray, const RenderMeshTriangle* hitrt, RayMeshIntersection& rmi) const;
	void scanBoundingBoxSpaceTreeNearestTriangle(const Ray& ray, RayMeshIntersection& rmi) const;
	float scanBoundingBoxRayBlocked(const Ray& ray, const float maxt, const RenderMeshTriangle* hitrt) const;

    bool putTriangleIntoChildrenNode(RaySpaceTreeNode* node, const RenderMeshTriangle* rt);
    bool putTriangleIntoTree(RaySpaceTreeNode* node, const RenderMeshTriangle* rt);
	void scanSpaceTreeNearestTriangle(const RaySpaceTreeNode* node, const Ray& ray, RayMeshIntersection& rmi) const;
    float scanSpaceTreeRayBlocked(const RaySpaceTreeNode* node, const Ray& ray, const float maxt, float* t_out = NULL) const;
	float scanBoundingBoxSpaceTreeRayBlocked(const Ray& ray, const float maxt, float* t_out = NULL) const;
	void scanSpaceTreeBoundingBox(const RaySpaceTreeNode* node, const Ray& ray,
																const RenderMeshTriangle* hitrt, RayMeshIntersection& rmi) const;
	void calcVertexInterpolation(const RenderMeshTriangle& rt, const vec3& hit, VertexInterpolation* hi) const;
    void calcVertexInterpolation(const RayTriangleIntersectionInfo& info, VertexInterpolation* vi) const;
    
	color3 traceAreaLight(const LightSource& lightSource, const vec3& hit, const vec3& normal) const;
	color3 tracePointLight(const LightSource& lightSource, const vec3& hit, const vec3& objectNormal) const;

	color4 renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y);
	color4 traceEyeRay(const Ray& ray) const;
    void traceEyeRaySurfaceInfo(const Ray& ray, ViewRaySurfaceInfo* info) const;

protected:
	RaySpaceTree tree;
	TriangleBVH bvh;
	
	std::vector<const RenderMeshTriangle*> triangleList;
	Image4f renderingImage;
	float progressRate = 0.0f;

	void transformScene();
	void transformObject(SceneTransformStack& transformStack, SceneObject& obj);
	void clearTransformedScene();
	
	std::map<const Mesh*, RayRenderTriangleList> meshTriangles;
    
    // ガイド付きデノイズ用バッファ（一次ヒット AOV）
    Image3f normalBuffer;
    Image3f albedoBuffer;
    Image3f depthBuffer;

    // Edge-avoiding À-Trous wavelet denoiser. Multi-pass with step sizes
    // 1, 2, 4, ... per level; guided by normal/depth AOVs.
    void denoiseImage(const Image3f& noisy, const Image3f& normal,
                      const Image3f& depth, const Image3f& albedo,
                      Image3f& output);
    void atrousPass(const Image3f& srcColor, Image3f& dstColor,
                    const Image3f& normal, const Image3f& depth,
                    int stepSize, int yStart, int yEnd) const;
    // Reinhard + ≈1/2.2 gamma applied in-place. Used as the post-denoise
    // step when linear-HDR denoising is active.
    void applyTonemapGamma(Image& img) const;
    
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
	color3 sampleEnvironment(const vec3& dir) const;
	// Importance-sample the envmap by luminance. u0/u1 are uniform in [0,1);
	// returns the sampled world-space direction in outDir and the solid-angle
	// PDF in outPdf. PDF is 0 when the envmap has no weight (no light to
	// sample) — callers must skip the NEE path then.
	void sampleEnvmapDirection(float u0, float u1, vec3& outDir, float& outPdf) const;
	// Solid-angle PDF that the envmap importance sampler would have assigned
	// to this world-space direction. Used on the BSDF side of the MIS pair.
	float envmapDirectionPdf(const vec3& dir) const;
	// Direct envmap contribution at a diffuse hit via luminance IS + shadow
	// ray + MIS against a cos-weighted BSDF strategy of pdf `bsdfPdf`.
	// Returns L(ω) · cos(θ) / π / pdf_env · w_env, excluding surface albedo
	// (caller multiplies). bsdfPdf=0 skips MIS (weight 1).
	color3 traceEnvmapLight(const vec3& hit, const vec3& normal, float bsdfPdf) const;
	color3 traceLight(const vec3& hit, const vec3& objectNormal) const;
	color3 lambertTraceLights(const vec3& hit, const vec3& objectNormal) const;

	// Effective area of the light strategy's pdf at a given emitter triangle:
	// returns triCount * tri.area, matching the "pick triangle uniformly,
	// then pick a uniform point" sampling used by traceAreaLight. Used by
	// the BSDF-sampled emission hit to reconstruct pdf_light for MIS.
	float areaLightSampledArea(const RenderMeshTriangle& tri) const;
    std::vector<LightSource> getAllLights() { return this->pointLightSources; }
    
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
	
	bool cullBackFace = false;
};

class RayShaderProvider
{
public:
	RayRenderer* renderer = NULL;

	RayShaderProvider(RayRenderer* renderer = NULL) : renderer(renderer) { }
	virtual ~RayShaderProvider() { }
	virtual color3 shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam = NULL) = 0;
};

class RaySimpleShaderProvider : public RayShaderProvider
{
	const vec3 lightSrc;

public:
	RaySimpleShaderProvider(RayRenderer* renderer)
		: RayShaderProvider(renderer)
		, lightSrc(normalize(vec3(1, 1, 1)))
	{ }
	
	color3 shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam = NULL) {
		const float n = 0.1f + fmaxf(dot(lightSrc, vi.normal), 0);
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

    color3 shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam = NULL) {
        const float c = clamp(this->renderer->calcAO(interInfo.hit, vi.normal, traceDistance), 0.0f, 1.0f);
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
	
	color3 shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam = NULL);
};

class RayBSDFBakeShaderProvider : public RayShaderProvider
{
	DiffuseShader diffuseShader;
	TransparencyShader transparencyShader;
	
public:
	RayBSDFBakeShaderProvider(RayRenderer* renderer = NULL)
	: RayShaderProvider(renderer) {
	}
	
	color3 shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray, const VertexInterpolation& vi, void* shaderParam = NULL);
};

}

#endif /* __ray_renderer_h__ */
