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
#include <atomic>
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

// Phase 4: an emissive participating-medium attached to a SceneObject is also
// a light source — registered here so surface and volume NEE can sample it.
// The medium pointer is just a borrow of the SceneObject's interiorMedium
// (Scene owns the lifetime); the bounding-mesh reference is what we use as
// the predicate to skip self-shadowing in shadow rays.
struct EmissiveVolumeSource {
	const SceneObject* object = NULL;
	const class HomogeneousMedium* medium = NULL;
	EmissiveVolumeSource() { }
};

// Screen-space tile rendered as a single work-stealing unit. Tiles are
// shuffled at render() start so progressive previews fill the frame
// approximately uniformly instead of top-to-bottom.
struct RenderTile {
	int x, y;
	int width, height;
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

	// Per-sample radiance clamp (“firefly clamp”). Bounds the HDR return of
	// each primary sample before accumulation so a single path with
	// near-infinite variance (small-radius NEE, glossy-caustic spikes, etc.)
	// can’t dominate the Monte-Carlo average. Introduces a small energy bias
	// on very bright features but cuts the speckle that otherwise never
	// converges. 0 disables. Interpreted in linear HDR radiance.
	float fireflyClamp = 10.0f;

	// Adaptive sampling: spend more samples on noisy tiles, fewer on
	// converged ones. Total per-pixel sample count is capped at `samples`,
	// but tiles whose relative standard error of the mean (rSEM) drops
	// below `adaptiveThreshold` stop early — yielding the same visual
	// quality faster on scenes with mixed difficulty (e.g. flat walls
	// next to a glass object). Off by default; enable via the viewer
	// "Adaptive" toggle or scene JSON.
	bool enableAdaptiveSampling = false;
	int adaptiveBaseSamples = 4;          // samples per pass
	float adaptiveThreshold = 0.02f;      // rSEM cap (lower = more accurate / slower)

	// Bloom runs in linear HDR radiance (pre-tonemap) so a tiny 10000-cd
	// emitter produces proportionally larger halo than a diffuse white pixel,
	// which a post-tonemap LDR bloom can't — both clamp to ~1 after Reinhard.
	float bloomThreshold = 1.0f;   // linear-radiance luma at which bloom starts
	float bloomStrength = 1.0f;    // additive gain on the blurred halo (HDR)
	float bloomRadius = 0.03f;     // halo sigma as fraction of main image width
	float bloomSizeAspect = 0.15f; // glow buffer size relative to main (performance; does not affect halo width)
	float bloomCurve = 1.0f;       // knee sharpness on excess ratio; 1=linear, >1=sharper

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
	std::vector<EmissiveVolumeSource> emissiveVolumeSources;
	
	void initRenderThreadContext(RenderThreadContext* ctx);
    void renderThread(const RenderThreadContext& ctx, const int threadId);
	void renderAsyncThread(RenderThreadCallback* callback);

	// Build the per-render tile list and shuffle it into a coverage-friendly
	// order. Called once per render() before the workers spawn.
	void buildTileList(int imgWidth, int imgHeight);

	// Adaptive driver: runs base + per-tile refinement passes, accumulating
	// into adaptiveSumImage / adaptiveSumSqImage and committing the running
	// mean to renderingImage / hdrImage so the preview refines progressively.
	void renderAdaptive(const RenderThreadContext& ctx);
	// Adaptive worker: pulls tile indices out of `activeTiles` via fetch_add,
	// runs samples [sampleStart, sampleStart + sampleCount) for each pixel.
	// `baseProgress` + `passShare` map per-pass tile completion onto a global
	// 0..1 progress scale so the bar advances monotonically across passes
	// (without these the per-pass `done/activeCount` would reset to 0 each
	// pass and never beat Pass 0's ceiling).
	void renderThreadAdaptive(const RenderThreadContext& ctx,
	                          const std::vector<size_t>* activeTiles,
	                          int sampleStart, int sampleCount,
	                          float baseProgress, float passShare);
	// Run a sample range for a pixel and accumulate per-sample HDR linear
	// radiance into caller-provided sum / sum-of-squares. AOVs (denoise
	// guides) are written only when sampleStart == 0. Used by both the
	// uniform path (one batched call) and adaptive (multiple passes).
	void accumulatePixelSamples(const RenderThreadContext& ctx, Ray& ray,
	                            int x, int y,
	                            int sampleStart, int sampleCount,
	                            color3f& sum, color3f& sumSq);
	// Commit the running mean for one tile to hdrImage + renderingImage so
	// the in-flight preview shows refinement after each adaptive pass.
	void commitTilePreview(const RenderThreadContext& ctx, size_t tileIdx);
	// Mean per-pixel relative standard-error-of-the-mean across the tile.
	// Returns 0 if the tile has no samples yet.
	float computeTileNoise(size_t tileIdx) const;
	
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

	color4 renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y, color4f* outHdr = NULL);
	color4 traceEyeRay(const Ray& ray) const;
    void traceEyeRaySurfaceInfo(const Ray& ray, ViewRaySurfaceInfo* info) const;

protected:
	RaySpaceTree tree;
	TriangleBVH bvh;
	
	std::vector<const RenderMeshTriangle*> triangleList;
	Image4f renderingImage;
	float progressRate = 0.0f;

	// Tile work-stealing state. nextTileIndex is the shared queue cursor;
	// completedTiles drives the progress callback. Both reset at render()
	// start. Workers atomically fetch_add nextTileIndex to claim a tile,
	// so a thread that finishes a heavy tile (lots of glass / deep
	// recursion) just grabs another while still-busy threads keep
	// grinding — much better balance than the old row-stride partition.
	std::vector<RenderTile> renderTiles;
	std::atomic<size_t> nextTileIndex{0};
	std::atomic<size_t> completedTiles{0};

	// Adaptive sampling state. Allocated and populated only when
	// settings.enableAdaptiveSampling is on; otherwise these stay empty
	// and zero overhead. adaptiveSumImage/SumSqImage are per-pixel
	// running totals (alpha unused). tileSampleCounts records the number
	// of samples that have been accumulated for each tile so far —
	// uniform across the tile, since every pass spans the whole tile.
	Image adaptiveSumImage;
	Image adaptiveSumSqImage;
	std::vector<int> tileSampleCounts;

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
    // Reinhard + ≈1/2.2 gamma. Reads linear HDR `src`, writes LDR `dst`.
    // Used as the final pass after HDR bloom (or as-is when bloom is off).
    void applyTonemapGamma(const Image& src, Image& dst) const;

    // Energy-based bloom — operates entirely on linear HDR radiance so a
    // 1-pixel 10000-cd source produces a halo proportional to (L-threshold),
    // not to the Reinhard-saturated ~1. Extracted out of render() so
    // reapplyPostProcess() can re-run it against the cached HDR image.
    void applyPostProcess(Image& hdr);

    // Linear HDR radiance buffer populated by each render thread alongside
    // the LDR `renderingImage` preview. Bloom + tonemap operate from here.
    Image hdrImage;

    // Snapshot of the (optionally denoised) linear HDR image taken right
    // before bloom. reapplyPostProcess restores from this so tweaking bloom
    // params is near-free compared to a full re-render.
    Image preBloomHdrImage;
    bool hasPreBloomImage = false;

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

	// BSDF-agnostic NEE sampling primitives: draw a direction from the light
	// strategy, shadow-test it, and hand back (dir, pdf_light, Le) so the
	// caller can evaluate its own BRDF·cos and compose the power-heuristic
	// MIS weight. Returns false when no sample is useful (no lights, back-
	// facing caller, back-facing light, or the shadow ray is blocked) — the
	// caller then contributes zero without a MIS pair. `pdf_light` is solid-
	// angle and matches the value traceAreaLight / traceEnvmapLight use on
	// their own MIS calculation.
	bool sampleAreaLightForNEE(const vec3& hit, const vec3& surfaceNormal,
	                           vec3& outDir, float& outPdfLight, color3& outLe) const;
	bool sampleEnvmapForNEE(const vec3& hit, const vec3& surfaceNormal,
	                        vec3& outDir, float& outPdfEnv, color3& outLi) const;
	// Phase 4: NEE for emissive participating media. Picks one of the
	// registered emissive volumes and equiangular-samples a point along its
	// cone axis (or bbox centre for Constant-mode media). On success returns
	//   outDir       — direction from `hit` to the sample point
	//   outDist      — distance along that direction
	//   outPdf       — solid-angle pdf (light-side of MIS)
	//   outLe        — σe at the sampled point, multiplied by medium tr along
	//                  the segment from hit to sample point (transmittance
	//                  through the volume itself; outside-segment Tr is left
	//                  to the caller's current medium)
	// Returns false when no emissive volumes exist, the geometry rejects
	// (passing through opaque geometry on the way), the sampled σe is zero,
	// or `surfaceNormal` faces away from outDir (caller can pass any normal-
	// like direction; pass -ray.dir for volumetric scatter sites).
	bool sampleVolumeLightForNEE(const vec3& hit, const vec3& surfaceNormal,
	                             vec3& outDir, float& outDist,
	                             float& outPdf, color3& outLe) const;
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
		this->hdrImage.createEmpty(width, height);
		// Pre-bloom cache was sized to the old buffer; drop it so the next
		// reapplyPostProcess call correctly falls back to a full render.
		this->hasPreBloomImage = false;
	}
  
    inline const Image& getRenderResult() const {
        return this->renderingImage;
    }

    // Linear-radiance HDR buffer (post-bloom, pre-tonemap). Float per channel,
    // un-clamped — bright emitters keep values >> 1.0. Save via writeHDR /
    // saveImage(...".hdr") to feed external compositors or HDR displays.
    inline const Image& getHdrResult() const {
        return this->hdrImage;
    }

	void clearRenderResult();

	// Re-runs post-process (bloom) on the cached pre-bloom image, skipping
	// the ray-tracing + denoise passes entirely. Returns false and leaves
	// renderingImage untouched if no prior render is cached yet.
	bool reapplyPostProcess();

	// Cooperative cancellation. Setting this from another thread makes the
	// current render() call bail out at the next row boundary and skip
	// denoise + bloom; the partial image in renderingImage is left intact so
	// the UI can still display it. Automatically cleared at the start of the
	// next render().
	std::atomic<bool> cancelRequested{false};
  
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
