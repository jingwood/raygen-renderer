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
#include <cassert>

#include "ugm/functions.h"
#include "ugm/imgfilter.h"
#include "ugm/imgcodec.h"
#include "ucm/ansi.h"
#include "lambert.h"
#include "polygons.h"

#define CUT_OFF_BACK_TRACE

#define AO_SAMPLES 50
#define AO_MAX_DISTANCE 10
#define AO_RANDOM_HEMISPHERE_RAY

#define TRACE_LIGHT_TRIES 1
#define TRACE_PATH_TRIES 1
// Safety cap on recursion. Russian Roulette handles typical path termination
// (see RayBSDFShaderProvider::shade), so this only bounds worst-case depth.
// Glass / refractive meshes can chain 10+ internal Fresnel bounces on
// concave geometry, so the cap needs headroom above the diffuse norm.
#define MAX_TRACE_DEPTH 32
// Start Russian Roulette after this many bounces so early, high-throughput
// bounces are always traced — only the tail of the path is stochastic.
#define MIN_RR_DEPTH 3
// Continuation probability is clamped to this range: low enough that dim
// paths can die quickly, high enough that variance from 1/q stays bounded.
#define RR_MIN_PROB 0.05f
#define RR_MAX_PROB 0.95f

#define PP_GLOW_SIZE_ASPECT 0.15
#define PP_GLOW_GAMMA 1.4
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
            //    this->shaderProvider = new LambertWithAOLightShaderProvider(this);
            break;
        case 5:
            this->shaderProvider = new RayBSDFShaderProvider(this);
            //    this->shaderProvider = new RayBSDFBakeShaderProvider(this);
            break;
    }
    
    this->renderingImage.setPixelDataFormat(PixelDataFormat::PDF_RGBA, 32);
    this->setRenderSize(this->settings.resolutionWidth, this->settings.resolutionHeight);

    this->cullBackFace = settings->cullBackFace;
    
    /* initialize random seed */
    srand((unsigned int)time(NULL));
}

RayRenderer::~RayRenderer() {
    if (this->shaderProvider != NULL) {
        delete this->shaderProvider;
        this->shaderProvider = NULL;
    }
    
    this->clearTransformedScene();
}

void RayRenderer::initRenderThreadContext(RenderThreadContext* ctx) {
    const Camera* camera = this->scene->mainCamera;
    if (camera == NULL) camera = &this->defaultCamera;
    
    const sizei& renderingImageSize = this->renderingImage.getSize();
    ctx->renderSize = sizef((float)renderingImageSize.width, (float)renderingImageSize.height);
    ctx->halfRenderSize = sizef(ctx->renderSize.width * 0.5f, ctx->renderSize.height * 0.5f);
    
    ctx->aspectRate = ctx->renderSize.width / ctx->renderSize.height;

    // Pinhole camera: fieldOfView is vertical FoV in degrees.
    // Clamp to < 180° since tan(90°) is undefined; use 179° as a safe upper bound.
    const float fovDeg = fminf(fmaxf(camera->fieldOfView, 1.0f), 179.0f);
    const float tanHalfFov = tanf(DEGREE_TO_RADIAN(fovDeg) * 0.5f);

    // dx/dy are angular offsets (tangent-space): at depth z, world offset = (dx*z, dy*z).
    // Pixels are square in screen space, so the per-pixel angular step is the
    // same in both axes — the overall horizontal FoV still widens with W/H
    // because the row simply has more pixels. Multiplying viewScaleX by the
    // aspect ratio on top of that (what the old code did) doubled up the
    // correction and stretched the image vertically by the aspect ratio.
    ctx->viewScaleY = (2.0f * tanHalfFov) / ctx->renderSize.height;
    ctx->viewScaleX = ctx->viewScaleY;

    // Legacy fields kept for any consumer that still reads them; not used by renderPixel.
    ctx->viewportSize = sizef(ctx->viewScaleX * ctx->renderSize.width,
                              ctx->viewScaleY * ctx->renderSize.height);

    ctx->depthOfField = camera->depthOfField;
    ctx->depthOfFieldScale = 1.0f;
    ctx->aperture = (camera->aperture > 0.0f) ? 1.0f / camera->aperture : 0.0f;
    ctx->halfAperture = ctx->aperture * 0.5f;
    ctx->apertureBlades = camera->apertureBlades;
    ctx->apertureRotation = camera->apertureRotation * (float)(M_PI / 180.0);
    ctx->exposure = camera->exposure;
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
    
    this->triangleList.clear();

    for (SceneObject* obj : this->scene->getObjects()) {
        if (obj->visible) {
            this->transformObject(*this->transformStack, *obj);
        }
    }
    
    this->bvh.build(this->triangleList);

    //    int count = 0;
    //    for (const auto& m : this->meshTriangles) {
    //        count += m.second.size();
    //    }
    //    printf("polygons: %d\n", count);
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
//        int count = 0;
        
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
                
                RenderMeshTriangle* rt = new RenderMeshTriangle(v1, v2, v3,
                                                              n1, n2, n3,
                                                              uv1, uv2, uv3,
                                                              uv4, uv5, uv6,
                                                              obj, *mesh);
                
//                rt->uvt2Info.shared.e1 = isSharedEdgeUV2(*mesh, k, uv5, uv4);
//                rt->uvt2Info.shared.e2 = isSharedEdgeUV2(*mesh, k, uv6, uv4);
//                rt->uvt2Info.shared.e3 = isSharedEdgeUV2(*mesh, k, uv6, uv5);
                
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
            const float s1 = sinf(RADIAN_TO_DEGREE(obj.angle.x));
            const float c1 = cosf(RADIAN_TO_DEGREE(obj.angle.x));
            const float s2 = sinf(RADIAN_TO_DEGREE(obj.angle.y));
            const float c2 = cosf(RADIAN_TO_DEGREE(obj.angle.y));
            
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

int calculateGaussianKernelSize(int width, int height) {
    int maxDim = std::max(width, height);

    // 画像サイズに応じたカーネルサイズ係数
    float factor = 0.01f;  // 1% をカーネルサイズとする目安
    int kernelSize = int(maxDim * factor);

    // 奇数に丸め、最小3、最大25に制限
    kernelSize = std::max(3, std::min(25, kernelSize | 1));  // 奇数へ

    return kernelSize;
}

void RayRenderer::render() {
    if (this->shaderProvider == NULL) return;
    
    this->resetTransformMatrices();
    
    if (this->scene == NULL || this->scene->mainCamera == NULL) return;

    RenderThreadContext ctx;
    this->initRenderThreadContext(&ctx);
    
    Scene& scene = *this->scene;
    Camera& camera = *scene.mainCamera;
    
    if (scene.mainCamera != NULL) {
        
        if (!camera.focusOnObjectName.isEmpty()) {
            const SceneObject* focusOnObj = scene.findObjectByName(camera.focusOnObjectName);
        
            if (focusOnObj) {
                
                BoundingBox bbox = focusOnObj->getBoundingBox();
//                const float size = fmaxf(bbox.size.x, fmaxf(bbox.size.y, bbox.size.z));
//                const vec3 ray = camera.getWorldLocation() - bbox.origin;
//                const vec3 dir = ray.normalize();
//
//                const float distance = size * 0.5 + size * 0.5f / tanf(camera.fieldOfView * 0.5f * M_PI / 180.f);
//
//                camera.location = bbox.origin + dir * distance;
                camera.lookAt(bbox.origin, vec3::up);
                
                vec3 focusPoint = bbox.origin;
                camera.depthOfField = length(camera.getWorldLocation() - focusPoint);
                
                const float length = fabsf(camera.viewFar - camera.viewNear);
                ctx.depthOfField = camera.depthOfField;
                ctx.depthOfFieldScale = (camera.depthOfField / length);
            }
        }
        
        this->applyCameraTransform(camera);
    }
    
    this->cameraWorldPos = camera.getWorldLocation();

    this->clearTransformedScene();
    this->transformScene();
    
    if (this->settings.enableDenoise) {
        this->normalBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
        this->depthBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
        this->albedoBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
    }

    this->progressRate = 0;

    std::vector<std::thread> threads;

    for (int i = 0; i < this->settings.threads; i++) {
        threads.push_back(std::thread([this, ctx, i] { this->renderThread(ctx, i); }));
    }

    for (std::thread &th : threads) {
        th.join();
    }

    if (this->settings.enableDenoise) {
        Image3f denoised;
        denoised.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
        this->denoiseImage(this->renderingImage, this->normalBuffer,
                           this->depthBuffer, this->albedoBuffer, denoised);
        Image::copy(denoised, this->renderingImage);
        this->applyTonemapGamma(this->renderingImage);
    }

    if (this->settings.enableRenderingPostProcess) {
        // Threshold at full resolution so sparse-pixel highlights survive —
        // downsampling before threshold bilinear-averages the bright pixel
        // with dim neighbours and drops it under the cutoff, killing bloom.
        Image glowimg(this->renderingImage.getPixelDataFormat(), 32);
        Image::copy(this->renderingImage, glowimg);

        const bool dump = !this->settings.postprocessDumpPath.isEmpty();
        auto dumpStage = [&](const char* tag, const Image& img) {
            if (!dump) return;
            ucm::string p = this->settings.postprocessDumpPath;
            p.appendFormat("-bloom-%s.jpg", tag);
            saveImage(img, p);
        };

        dumpStage("00-input", this->renderingImage);
        img::thresholdSoft(glowimg, this->settings.bloomThreshold, this->settings.bloomCurve);
        dumpStage("01-threshold", glowimg);
        img::gamma(glowimg, PP_GLOW_GAMMA);
        dumpStage("02-gamma", glowimg);
        glowimg.resize((int)((float)this->renderingImage.width() * this->settings.bloomSizeAspect),
            (int)((float)this->renderingImage.height() * this->settings.bloomSizeAspect));
        dumpStage("03-downsample", glowimg);
        int kernelSize = calculateGaussianKernelSize(glowimg.width(), glowimg.height());
        img::gaussBlur(glowimg, kernelSize);
        dumpStage("04-blur", glowimg);
        glowimg.resize(this->renderingImage.getSize());
        dumpStage("05-upsample", glowimg);
        // Straight additive composite — light is physically additive. The
        // earlier Lighter blend (oc = c1 + max(c2-c1,0)·factor) dropped the
        // halo wherever the main image was already bright (walls, metal),
        // so bloom only showed on dark background pixels.
        img::calc(this->renderingImage, glowimg, img::CalcMethods::Add, this->settings.bloomStrength);
        dumpStage("06-composite", this->renderingImage);
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

color4f RayRenderer::renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y) {

    // Guided-denoise AOVs (primary hit only). Miss pixels get sentinels so
    // the denoiser's depth weight naturally isolates background from geometry.
    if (this->settings.enableDenoise) {
        ViewRaySurfaceInfo traceRayInfo;
        this->traceEyeRaySurfaceInfo(ray, &traceRayInfo);

        if (traceRayInfo.hitted) {
            // Normal encoded [-1..1] → [0..1]
            const vec3 normalColor = traceRayInfo.hi.normal * 0.5f + 0.5f;
            this->normalBuffer.setPixel(x, y, color4(normalColor, 1.0f));

            // Albedo from material base color (texture sample not folded in here;
            // demodulation is a future improvement and would need linear HDR).
            this->albedoBuffer.setPixel(x, y, color4(traceRayInfo.mat->color, 1.0f));

            // Depth: near = 1, far = 0 (sqrt-compressed for perceptual spacing)
            const float distance = (traceRayInfo.interInfo.hit - cameraWorldPos).length();
            float depth = distance / scene->mainCamera->viewFar;
            depth = sqrtf(depth);
            depth = 1.0f - clamp(depth, 0.0f, 1.0f);
            this->depthBuffer.setPixel(x, y, color4(depth, depth, depth, 1.0f));
        } else {
            // Background sentinel: encoded-zero normal, zero depth (= far).
            this->normalBuffer.setPixel(x, y, color4(0.5f, 0.5f, 0.5f, 0.0f));
            this->albedoBuffer.setPixel(x, y, this->settings.backColor);
            this->depthBuffer.setPixel(x, y, color4(0.0f, 0.0f, 0.0f, 0.0f));
        }
    }


    // Angular offsets (tangent-space). Ray through pixel is normalize(vec3(dx, dy, -1)).
    const float dx = ((float)x + 0.5f - ctx.halfRenderSize.width) * ctx.viewScaleX;
    const float dy = -((float)y + 0.5f - ctx.halfRenderSize.height) * ctx.viewScaleY;

    color4f sampleColor;
    const int totalSamples = this->settings.samples;
    const bool aaEnabled = this->settings.enableAntialias;

    for (int i = 0; i < totalSamples; i++) {
        // Reset the Halton walk for this (pixel, sample). The early dims (0,1
        // for sub-pixel jitter, 2,3 for DOF) are the best-stratified slots, and
        // the remaining dims propagate down into the path trace for BSDF /
        // light sampling.
        ldsBeginPixelSample(x, y, i);

        if (ctx.depthOfField >= 0.001f && ctx.aperture > 0.0f) {
            // Sub-pixel jitter on dims 0,1 when AA is on; pin to pixel centre
            // otherwise. When AA is off we still consume the 2D sample so the
            // Halton dims line up with the AA-on case and DOF/path sampling
            // sees the same downstream stratification.
            float jx, jy;
            ldsNext2D(jx, jy);
            const float jxOffset = aaEnabled ? (jx - 0.5f) : 0.0f;
            const float jyOffset = aaEnabled ? (jy - 0.5f) : 0.0f;
            const float pxDx = dx + jxOffset * ctx.viewScaleX;
            const float pxDy = dy - jyOffset * ctx.viewScaleY;
            const vec3 focalPointJ(pxDx * ctx.depthOfField, pxDy * ctx.depthOfField, -ctx.depthOfField);

            // Aperture sample on dims 2,3. Blades=0 → full disk (circular
            // bokeh). Blades ≥ 3 → uniformly sample a regular n-gon inscribed
            // in the aperture radius so out-of-focus highlights take the
            // familiar hex/octagon iris shape of real lenses. Sampling is
            // done wedge-by-wedge: pick a triangle from the centre, then a
            // uniform point inside it (square → triangle fold).
            float du, dv;
            ldsNext2D(du, dv);
            float offsetX, offsetY;
            if (ctx.apertureBlades >= 3) {
                const int N = ctx.apertureBlades;
                const float u0 = du * (float)N;
                const int wedge = fminf((float)(N - 1), floorf(u0));
                float s = u0 - (float)wedge;
                float t = dv;
                if (s + t > 1.0f) { s = 1.0f - s; t = 1.0f - t; }
                const float step = 2.0f * (float)M_PI / (float)N;
                const float a0 = ctx.apertureRotation + step * (float)wedge;
                const float a1 = a0 + step;
                const float px = cosf(a0) * s + cosf(a1) * t;
                const float py = sinf(a0) * s + sinf(a1) * t;
                offsetX = px * ctx.aperture;
                offsetY = py * ctx.aperture;
            } else {
                // Shirley's concentric square-to-disk mapping — keeps
                // stratified (u,v) grid cells compact when mapped to the
                // disk. The older r = √u, θ = 2πv path produced radial
                // spokes because it stretched each cell along the radius.
                const float a = 2.0f * du - 1.0f;
                const float b = 2.0f * dv - 1.0f;
                float r, phi;
                if (a == 0.0f && b == 0.0f) {
                    r = 0.0f; phi = 0.0f;
                } else if (a * a > b * b) {
                    r = a;
                    phi = (float)(M_PI / 4.0) * (b / a);
                } else {
                    r = b;
                    phi = (float)(M_PI / 2.0) - (float)(M_PI / 4.0) * (a / b);
                }
                offsetX = r * cosf(phi) * ctx.aperture;
                offsetY = r * sinf(phi) * ctx.aperture;
            }

            ray.origin = vec3(offsetX, offsetY, 0.0f);
            ray.dir = (focalPointJ - ray.origin).normalize();
        } else {
            // Sub-pixel jitter for stochastic anti-aliasing; ray direction
            // pivots at origin. When AA is off we still consume the 2D LDS
            // sample (see note above) but ignore it, so the ray goes through
            // the deterministic pixel centre and edges will alias.
            float jx, jy;
            ldsNext2D(jx, jy);
            const float jxOffset = aaEnabled ? (jx - 0.5f) : 0.0f;
            const float jyOffset = aaEnabled ? (jy - 0.5f) : 0.0f;
            ray.origin = vec3::zero;
            ray.dir = vec3(dx + jxOffset * ctx.viewScaleX,
                           dy - jyOffset * ctx.viewScaleY,
                           -1.0f).normalize();
        }

        sampleColor += this->traceEyeRay(ray);
    }

    const color3f radiance = sampleColor * ctx.exposure / (float)totalSamples;

    // When the denoiser is on we defer tonemap+gamma to a post-denoise pass:
    // filtering in linear radiance avoids the banding that non-linear
    // compression induces around edges and gradients.
    if (this->settings.enableDenoise) {
        return color4f(fmaxf(radiance.r, 0.0f),
                       fmaxf(radiance.g, 0.0f),
                       fmaxf(radiance.b, 0.0f),
                       1.0f);
    }

    // Luminance-based Reinhard: compress the perceived brightness L = luma
    // and scale RGB by the same factor so saturated colors stay saturated.
    // Per-channel Reinhard (the old approach) desaturated bright mixed
    // colors toward white because it shrinks the high channels more than
    // the low ones (10:3:1 → 0.91:0.75:0.5, washed out). If the rescaled
    // RGB exceeds [0,1] gamut (pure saturated HDR red like (10,0,0)), clip
    // by dividing by the peak channel so the hue is preserved and only
    // brightness tops out at 1.
    const float L = 0.2126f * radiance.r + 0.7152f * radiance.g + 0.0722f * radiance.b;
    float mr = 0.0f, mg = 0.0f, mb = 0.0f;
    if (L > 1e-6f) {
        const float Lmapped = L / (L + 1.0f);
        const float scale = Lmapped / L;
        mr = radiance.r * scale;
        mg = radiance.g * scale;
        mb = radiance.b * scale;
        const float peak = fmaxf(fmaxf(mr, mg), mb);
        if (peak > 1.0f) {
            const float inv = 1.0f / peak;
            mr *= inv; mg *= inv; mb *= inv;
        }
    }
    const float invGamma = 1.0f / 2.2f;
    const color3f encoded(powf(fmaxf(mr, 0.0f), invGamma),
                          powf(fmaxf(mg, 0.0f), invGamma),
                          powf(fmaxf(mb, 0.0f), invGamma));
    return clamp(encoded, 0.0f, 1.0f);
}

color4 RayRenderer::traceEyeRay(const Ray& ray) const {

//    RayMeshIntersection rmi(NULL, 9999999.0f);
    RayTriangleIntersectionInfo interInfo;
    this->findNearestTriangle(ray, interInfo);

    if (interInfo.triangle != NULL) {
        VertexInterpolation vi;
        this->calcVertexInterpolation(interInfo, &vi);

        if (interInfo.triangle->object.visible) {
            // Return HDR radiance unclamped so high-intensity emitters (e.g.
            // a red light with emission=10) carry through to the tonemap.
            // Reinhard at renderPixel's output handles the compression.
            const color3 shaded = this->shaderProvider->shade(interInfo, ray, vi);
            return color4(fmaxf(shaded.r, 0.0f),
                          fmaxf(shaded.g, 0.0f),
                          fmaxf(shaded.b, 0.0f),
                          1.0f);
        }
    }

    const color3 env = this->sampleEnvironment(ray.dir);
    if (env != color3::zero) {
        return color4(env, 1.0f);
    }
    return this->settings.backColor;
}

color3 RayRenderer::sampleEnvironment(const vec3& dir) const {
    if (this->scene == NULL) return color3::zero;

    const vec3 d = dir.normalize();
    const float yaw = this->scene->envmapRotation * (float)(M_PI / 180.0);
    const float cosY = cosf(yaw), sinY = sinf(yaw);
    // Rotate the sample direction around Y before looking up.
    const float rx = d.x * cosY - d.z * sinY;
    const float rz = d.x * sinY + d.z * cosY;
    const float ry = d.y;

    // Cubemap takes precedence when all six faces are present.
    Texture* const* faces = this->scene->envCubemapFaces;
    if (faces[0] != NULL && faces[1] != NULL && faces[2] != NULL
        && faces[3] != NULL && faces[4] != NULL && faces[5] != NULL) {
        // OpenGL cubemap convention. Select the face by the dominant axis
        // and project the other two into [0, 1] texture coordinates. Signs
        // below match the typical px/nx/py/ny/pz/nz prefix asset layout.
        const float ax = fabsf(rx), ay = fabsf(ry), az = fabsf(rz);
        int face; float sc, tc, ma;
        if (ax >= ay && ax >= az) {
            ma = ax;
            if (rx > 0) { face = 0; sc = -rz; tc =  ry; }  // +X
            else        { face = 1; sc =  rz; tc =  ry; }  // -X
        } else if (ay >= ax && ay >= az) {
            ma = ay;
            if (ry > 0) { face = 2; sc =  rx; tc =  rz; }  // +Y
            else        { face = 3; sc =  rx; tc = -rz; }  // -Y
        } else {
            ma = az;
            if (rz > 0) { face = 4; sc =  rx; tc =  ry; }  // +Z
            else        { face = 5; sc = -rx; tc =  ry; }  // -Z
        }
        const float u = 0.5f * (sc / ma + 1.0f);
        // Image storage is top-down (row 0 at top), so v = 0 should be world
        // up. Side-face tc carries +ry directly; v = 0.5·(1 − tc/ma) then
        // maps ry > 0 (world up) to v < 0.5 (upper rows of the image).
        const float v = 0.5f * (1.0f - tc / ma);
        return faces[face]->sample(vec2(u, v)).rgb * this->scene->envmapIntensity;
    }

    if (this->scene->envmap == NULL) return color3::zero;

    // Equirectangular (lat-long) mapping: u from azimuth, v from elevation.
    const float u = 0.5f + atan2f(rz, rx) * (float)(0.5 / M_PI);
    const float v = 0.5f - asinf(clamp(ry, -1.0f, 1.0f)) * (float)(1.0 / M_PI);

    const color3 sample = this->scene->envmap->sample(vec2(u, v)).rgb;
    return sample * this->scene->envmapIntensity;
}

namespace {
// Binary search in an ascending CDF whose last element is 1.0. Returns the
// largest index i with cdf[i] <= u, clamped to [0, n-1]. `n` is the number
// of intervals (cdf has n+1 entries).
inline int searchCDF(const float* cdf, int n, float u) {
    int lo = 0, hi = n;
    while (lo < hi) {
        const int mid = (lo + hi) >> 1;
        if (cdf[mid + 1] <= u) lo = mid + 1; else hi = mid;
    }
    return lo < n ? lo : n - 1;
}
}

namespace {
// Invert the (face, s, t) → direction mapping used by sampleEnvironment.
// s = 2u - 1, t = 1 - 2v. Direction is un-normalised so caller can normalise.
inline ugm::vec3 cubeFaceDir(int face, float s, float t) {
    switch (face) {
        case 0: return ugm::vec3(+1.0f,    t,   -s);
        case 1: return ugm::vec3(-1.0f,    t,    s);
        case 2: return ugm::vec3(   s, +1.0f,    t);
        case 3: return ugm::vec3(   s, -1.0f,   -t);
        case 4: return ugm::vec3(   s,    t, +1.0f);
        default: return ugm::vec3(  -s,    t, -1.0f);
    }
}
}

void RayRenderer::sampleEnvmapDirection(float u0, float u1, vec3& outDir, float& outPdf) const {
    outDir = vec3(0.0f, 1.0f, 0.0f);
    outPdf = 0.0f;
    if (this->scene == NULL) return;

    // Cubemap path: sample (face, y, x) from the three-level CDF.
    if (this->scene->envCubeFaceSize > 0 && this->scene->envCubeTotalWeight > 0.0f) {
        const int W = this->scene->envCubeFaceSize;
        const int H = W;

        const int face = searchCDF(this->scene->envCubeMarginalFace.data(), 6, u0);
        const float faceLo = this->scene->envCubeMarginalFace[face];
        const float faceSpan = this->scene->envCubeMarginalFace[face + 1] - faceLo;
        const float u0r = (faceSpan > 0.0f) ? (u0 - faceLo) / faceSpan : 0.5f;

        const float* marY = &this->scene->envCubeMarginalY[(size_t)face * (H + 1)];
        const int y = searchCDF(marY, H, u0r);
        const float yLo = marY[y];
        const float ySpan = marY[y + 1] - yLo;
        const float yFrac = (ySpan > 0.0f) ? (u0r - yLo) / ySpan : 0.5f;

        const float* condX = &this->scene->envCubeConditionalX[((size_t)face * H + y) * (W + 1)];
        const int x = searchCDF(condX, W, u1);
        const float xLo = condX[x];
        const float xSpan = condX[x + 1] - xLo;
        const float xFrac = (xSpan > 0.0f) ? (u1 - xLo) / xSpan : 0.5f;

        const float u = (x + xFrac) / (float)W;
        const float v = (y + yFrac) / (float)H;
        const float s = 2.0f * u - 1.0f;
        const float t = 1.0f - 2.0f * v;

        vec3 dirLocal = cubeFaceDir(face, s, t);

        // Undo envmap rotation so the returned direction is world-space.
        const float yaw = this->scene->envmapRotation * (float)(M_PI / 180.0);
        const float cosY = cosf(yaw), sinY = sinf(yaw);
        const float wx =  dirLocal.x * cosY + dirLocal.z * sinY;
        const float wz = -dirLocal.x * sinY + dirLocal.z * cosY;
        outDir = vec3(wx, dirLocal.y, wz).normalize();

        // PDF in solid-angle: p(ω) = lum(texel)·jac / totalWeight, where jac
        // = 1/(s² + t² + 1)^(3/2) is the per-texel solid-angle Jacobian and
        // the CDF was built on (lum × jac) weights.
        const color4f texel = this->scene->envCubemapFaces[face]->getImage().getPixel(x, y);
        const float lum = fmaxf(0.0f, 0.2126f * texel.r + 0.7152f * texel.g + 0.0722f * texel.b);
        const float denom = s * s + t * t + 1.0f;
        const float jac = 1.0f / (denom * sqrtf(denom));
        // Convert from "pdf per (u,v) face area" to solid-angle pdf: multiply
        // by Jacobian of (u,v → ω). Area of one texel in (s,t) space = 4/(W·H),
        // so pdf_uv_area = lum·jac / totalWeight, and pdf_ω = pdf_uv_area /
        // (jac · 4/(W·H)) = lum · W·H / (4 · totalWeight).
        outPdf = lum * (float)(W * H) / (4.0f * this->scene->envCubeTotalWeight);
        return;
    }

    const int W = this->scene->envmapW;
    const int H = this->scene->envmapH;
    if (W <= 0 || H <= 0 || this->scene->envmapTotalWeight <= 0.0f) return;

    // 2D inversion: first the row from the marginal, then a pixel within
    // that row from its conditional CDF.
    const int y = searchCDF(this->scene->envmapMarginalY.data(), H, u0);
    const float* row = &this->scene->envmapConditionalX[(size_t)y * (W + 1)];
    const int x = searchCDF(row, W, u1);

    // Jitter inside the chosen pixel so samples aren't all on pixel centres.
    const float yPrev = this->scene->envmapMarginalY[y];
    const float ySpan = this->scene->envmapMarginalY[y + 1] - yPrev;
    const float xPrev = row[x];
    const float xSpan = row[x + 1] - xPrev;
    const float yFrac = (ySpan > 0.0f) ? (u0 - yPrev) / ySpan : 0.5f;
    const float xFrac = (xSpan > 0.0f) ? (u1 - xPrev) / xSpan : 0.5f;

    const float u = (x + xFrac) / (float)W;
    const float v = (y + yFrac) / (float)H;

    // (u, v) → direction. Mirror sampleEnvironment's mapping and un-rotate
    // so the direction is in world space.
    const float phi = (u - 0.5f) * 2.0f * (float)M_PI;
    const float theta = (0.5f - v) * (float)M_PI;
    const float cosT = cosf(theta), sinT = sinf(theta);
    float dx = cosT * cosf(phi);
    const float dy = sinf(theta);
    float dz = cosT * sinf(phi);

    // sampleEnvironment does: rx = x*cosY - z*sinY, rz = x*sinY + z*cosY.
    // To undo that, rotate (dx, dz) by -yaw.
    const float yaw = this->scene->envmapRotation * (float)(M_PI / 180.0);
    const float cosY = cosf(yaw), sinY = sinf(yaw);
    const float wx = dx * cosY + dz * sinY;
    const float wz = -dx * sinY + dz * cosY;
    outDir = vec3(wx, dy, wz).normalize();

    // PDF conversion: from (u,v) uniform space to solid angle on the sphere.
    // p_img(u,v) = pixel_weight / total_weight.
    // p_dir(ω) = p_img * (W*H) / (2π² sin(θ))  [standard eqrect jacobian]
    const color4f texel = this->scene->envmap->getImage().getPixel(x, y);
    const float lum = fmaxf(0.0f, 0.2126f * texel.r + 0.7152f * texel.g + 0.0722f * texel.b);
    const float sinTheta = sinf((float)M_PI * v);
    if (sinTheta <= 0.0f) { outPdf = 0.0f; return; }
    const float pdfUV = (lum * sinTheta) / this->scene->envmapTotalWeight;
    outPdf = pdfUV * (float)(W * H) / (2.0f * (float)(M_PI * M_PI) * sinTheta);
}

color3 RayRenderer::traceEnvmapLight(const vec3& hit, const vec3& normal, float bsdfPdf) const {
    if (this->scene == NULL) return color3::zero;
    const bool hasEquirect = this->scene->envmap != NULL && this->scene->envmapTotalWeight > 0.0f;
    const bool hasCube = this->scene->envCubeFaceSize > 0 && this->scene->envCubeTotalWeight > 0.0f;
    if (!hasEquirect && !hasCube) return color3::zero;

    vec3 envDir;
    float envPdf = 0.0f;
    this->sampleEnvmapDirection(randomValue(), randomValue(), envDir, envPdf);
    if (envPdf <= 0.0f) return color3::zero;

    const float cosObj = dot(envDir, normal);
    if (cosObj <= 0.0f) return color3::zero;

    const Ray shadowRay = ThicknessRay(hit, envDir);
    const bool blocked = this->bvh.intersectAny(shadowRay, 1e30f, [](const RenderMeshTriangle* rt) {
        const auto& mat = rt->object.material;
        if (mat.emission > 0.0f) return false;
        return mat.transparency < 0.01f || mat.refraction > 0.1f;
    });
    if (blocked) return color3::zero;

    // Power heuristic MIS weight. bsdfPdf is the cosine-weighted strategy's
    // pdf for this direction = cos/π. When the caller didn't advertise MIS
    // (bsdfPdf = 0) use a weight of 1 — standalone NEE, no pair.
    float w = 1.0f;
    if (bsdfPdf > 0.0f) {
        const float e2 = envPdf * envPdf;
        const float b2 = bsdfPdf * bsdfPdf;
        w = e2 / (e2 + b2);
    }

    const color3 Li = this->sampleEnvironment(envDir);
    // Lambertian BRDF = 1/π; cosθ from shading; divide by pdf_env; apply MIS.
    return Li * (cosObj * w / ((float)M_PI * envPdf));
}

float RayRenderer::envmapDirectionPdf(const vec3& dir) const {
    if (this->scene == NULL) return 0.0f;

    const vec3 d = dir.normalize();
    const float yaw = this->scene->envmapRotation * (float)(M_PI / 180.0);
    const float cosY = cosf(yaw), sinY = sinf(yaw);
    const float rx = d.x * cosY - d.z * sinY;
    const float rz = d.x * sinY + d.z * cosY;

    if (this->scene->envCubeFaceSize > 0 && this->scene->envCubeTotalWeight > 0.0f) {
        // Mirror sampleEnvironment's face selection and project to (s, t).
        const float ax = fabsf(rx), ay = fabsf(d.y), az = fabsf(rz);
        int face; float s, t, ma;
        if (ax >= ay && ax >= az) {
            ma = ax;
            if (rx > 0) { face = 0; s = -rz; t =  d.y; }
            else        { face = 1; s =  rz; t =  d.y; }
        } else if (ay >= ax && ay >= az) {
            ma = ay;
            if (d.y > 0) { face = 2; s = rx; t = rz; }
            else         { face = 3; s = rx; t = -rz; }
        } else {
            ma = az;
            if (rz > 0) { face = 4; s =  rx; t =  d.y; }
            else        { face = 5; s = -rx; t =  d.y; }
        }
        if (ma <= 0.0f) return 0.0f;
        const float u = 0.5f * (s / ma + 1.0f);
        const float v = 0.5f * (1.0f - t / ma);
        const int W = this->scene->envCubeFaceSize;
        int x = (int)(u * W); if (x < 0) x = 0; if (x >= W) x = W - 1;
        int y = (int)(v * W); if (y < 0) y = 0; if (y >= W) y = W - 1;
        const color4f texel = this->scene->envCubemapFaces[face]->getImage().getPixel(x, y);
        const float lum = fmaxf(0.0f, 0.2126f * texel.r + 0.7152f * texel.g + 0.0722f * texel.b);
        return lum * (float)(W * W) / (4.0f * this->scene->envCubeTotalWeight);
    }

    const int W = this->scene->envmapW;
    const int H = this->scene->envmapH;
    if (W <= 0 || H <= 0 || this->scene->envmapTotalWeight <= 0.0f) return 0.0f;

    const float u = 0.5f + atan2f(rz, rx) * (float)(0.5 / M_PI);
    const float v = 0.5f - asinf(clamp(d.y, -1.0f, 1.0f)) * (float)(1.0 / M_PI);
    int x = (int)(u * W); if (x < 0) x = 0; if (x >= W) x = W - 1;
    int y = (int)(v * H); if (y < 0) y = 0; if (y >= H) y = H - 1;

    const color4f texel = this->scene->envmap->getImage().getPixel(x, y);
    const float lum = fmaxf(0.0f, 0.2126f * texel.r + 0.7152f * texel.g + 0.0722f * texel.b);
    const float sinTheta = sinf((float)M_PI * v);
    if (sinTheta <= 0.0f) return 0.0f;
    const float pdfUV = (lum * sinTheta) / this->scene->envmapTotalWeight;
    return pdfUV * (float)(W * H) / (2.0f * (float)(M_PI * M_PI) * sinTheta);
}

void RayRenderer::traceEyeRaySurfaceInfo(const Ray& ray, ViewRaySurfaceInfo* surfaceInfo) const {

    RayTriangleIntersectionInfo& interInfo = surfaceInfo->interInfo;
    this->findNearestTriangle(ray, interInfo);

    if (interInfo.triangle != NULL) {
        VertexInterpolation hi;
        this->calcVertexInterpolation(interInfo, &hi);

        if (interInfo.triangle->object.visible) {
            surfaceInfo->hitted = true;
            surfaceInfo->interInfo = interInfo;
            surfaceInfo->hi = hi;
            surfaceInfo->mat = &interInfo.triangle->object.material;
            return;
        }
    }

    surfaceInfo->hitted = false;
}

color3 RayRenderer::tracePath(const Ray& ray, void* shaderParam) const {
    RayTriangleIntersectionInfo info;
    this->findNearestTriangle(ray, info);

    if (info.triangle != NULL) {
        VertexInterpolation vi;
        this->calcVertexInterpolation(info, &vi);

        return this->shaderProvider->shade(info, ray, vi, shaderParam);
    }

    // Ray escaped the scene — treat the environment map as distant radiance
    // from every direction. Falls back to zero when no envmap is set.
    color3 env = this->sampleEnvironment(ray.dir);

    // If the caller advertised envmap MIS (i.e. sampled this direction with
    // a cos-weighted diffuse lobe), apply the power-heuristic weight so
    // direct NEE via traceEnvmapLight does not double-count.
    if (shaderParam != NULL) {
        const BSDFParam* sp = (const BSDFParam*)shaderParam;
        if (sp->misEnvBsdfPdf > 0.0f) {
            const float envPdf = this->envmapDirectionPdf(ray.dir);
            const float b2 = sp->misEnvBsdfPdf * sp->misEnvBsdfPdf;
            const float e2 = envPdf * envPdf;
            const float w = (b2 + e2 > 0.0f) ? b2 / (b2 + e2) : 1.0f;
            env = env * w;
        }
    }
    return env;
}

void RayRenderer::findNearestTriangle(const Ray& ray, RayTriangleIntersectionInfo& info) const {
    this->bvh.intersectClosest(ray, info);
}

vec3 cosineWeightedPointInTriangle(const Triangle& tri, const vec3& normal) {
    // まず三角形上のランダム点を取得（Barycentric Coordinates）
    float u = randomValue();
    float v = randomValue();

    if (u + v > 1.0f) {
        u = 1.0f - u;
        v = 1.0f - v;
    }

    vec3 p = tri.v1 + (tri.v2 - tri.v1) * u + (tri.v3 - tri.v1) * v;

    // コサイン加重分布のために法線に従ってサンプルを重み付け（微調整として最小限）
    vec3 tangent = normalize(cross(normal, vec3(0.0072f, 1.0f, 0.0034f)));
    vec3 bitangent = cross(normal, tangent);

    // 円形分布に基づく局所座標系内でのオフセットを作成
    float r = sqrtf(randomValue());
    float theta = 2.0f * M_PI * randomValue();

    float x = r * cosf(theta);
    float y = r * sinf(theta);

    vec3 offset = tangent * x + bitangent * y;
    vec3 cosinePoint = p + offset * 0.001f; // 小さなオフセットで重みを近似

    return cosinePoint;
}

float lightPDF(const vec3& hitPoint, const vec3& lightPoint, const vec3& lightNormal, float lightArea) {
    vec3 dir = normalize(lightPoint - hitPoint);
    float distanceSquared = (lightPoint - hitPoint).lengthSquared();
    float cosThetaLight = dot(-dir, lightNormal);
    
    if (cosThetaLight <= 0.0f) return 0.0f; // 背面はサンプルできない

    // 面積から立体角への変換
    return distanceSquared / (cosThetaLight * lightArea);
}

float RayRenderer::areaLightSampledArea(const RenderMeshTriangle& tri) const {
    auto it = this->meshTriangles.find(&tri.mesh);
    if (it == this->meshTriangles.end()) return tri.area;
    return (float)it->second.size() * tri.area;
}

color3 RayRenderer::traceAreaLight(const LightSource& lightSource, const vec3& hit, const vec3& objectNormal) const {
    const SceneObject* obj = lightSource.object;
    if (obj == NULL) return color3::zero;

    const auto& meshes = obj->getMeshes();
    if (meshes.size() <= 0) return color3::zero;

    const Mesh* mesh = meshes[rand() % meshes.size()];

    const auto& triangleList = this->meshTriangles.at(mesh);
    if (triangleList.size() <= 0) return colors::transparent;

    const size_t triCount = triangleList.size();
    const auto& triangle = *triangleList[rand() % triCount];

    // Uniform area sampling: picking a triangle with probability 1/N and then
    // a point uniformly within it gives point-pdf 1/(N * triArea). Converted to
    // solid angle (multiplied by r²/cos_light), the estimator carries a factor
    // of (N * triArea * cos_light / r²).
    const vec3 p = ldsPointInTriangle(triangle.tri);
    const vec3 lightRay = p - hit;
    const vec3 lightDir = normalize(lightRay);

    const float dotObjectToLight = dot(lightDir, objectNormal);
    if (dotObjectToLight <= 0.0f) return color3::zero;

    VertexInterpolation lightHit;
    calcVertexInterpolation(triangle, p, &lightHit);

    const float dotLightToRay = dot(-lightDir, lightHit.normal);
    if (dotLightToRay <= 0.0f) return color3::zero;

    Ray ray = ThicknessRay(hit, lightRay);
    constexpr float maxt = 0.99999f;

    // Any opaque non-emissive triangle between shading point and the sampled
    // light point occludes the contribution. `maxt < 1` clips before the
    // exact sampled point, but area lights are typically multi-triangle, so
    // other triangles of the same light can still sit at t<1 — skip anything
    // emissive so a light never self-shadows.
    const bool blocked = this->bvh.intersectAny(ray, maxt, [](const RenderMeshTriangle* rt) {
        const auto& mat = rt->object.material;
        return mat.transparency < 0.01f && mat.emission <= 0.0f;
    });

    if (blocked) return color3::zero;

    const Material& lightMat = lightSource.object->material;
    const float sampledArea = (float)triCount * triangle.area;
    const float r2 = lightRay.length2();

    // MIS power heuristic (β=2) between this shadow-ray (light) strategy and
    // the cos-weighted BSDF strategy that could have sampled the same
    // direction. pdf_light is in solid-angle measure: r²/(cos_light · area).
    // pdf_bsdf assumes the caller is Lambertian (cos_obj/π). On diffuse
    // paths the two strategies now sum to ~1 instead of double-counting.
    const float pdfLight = r2 / (dotLightToRay * sampledArea);
    const float pdfBsdf = dotObjectToLight / (float)M_PI;
    const float pdfLight2 = pdfLight * pdfLight;
    const float pdfBsdf2 = pdfBsdf * pdfBsdf;
    const float wLight = pdfLight2 / (pdfLight2 + pdfBsdf2);

    // Full direct-light term including the Lambert BRDF's 1/π; the shader
    // multiplies by surface albedo. Equivalent to:
    //   L_direct = (albedo/π) * Li * cos_obj * cos_light * area / r²
    return lightMat.color * lightMat.emission
        * (dotObjectToLight * dotLightToRay * sampledArea / ((float)M_PI * r2))
        * wLight;
}

color3 RayRenderer::tracePointLight(const LightSource& lightSource, const vec3& hit, const vec3& objectNormal) const {
    const vec3 lightray = lightSource.transformedLocation - hit;

    Ray ray = ThicknessRay(hit, lightray);
    constexpr float maxt = 0.99999f;

    const bool blocked = this->bvh.intersectAny(ray, maxt, [](const RenderMeshTriangle* rt) {
        const auto& mat = rt->object.material;
        if (mat.emission > 0.0f) return false;
        return mat.transparency < 0.01f || mat.refraction > 0.1f;
    });

    const SceneObject* light = lightSource.object;

    if (!blocked) {
        const vec3 lightrayNormal = lightray.normalize();

        float dotToObject = dot(lightrayNormal, objectNormal);
        float dotToLight = dot(lightrayNormal, lightSource.transformedNormal);

        if (dotToObject > 0) {
            const Material& lightMat = light->material;

            if (lightMat.spotRange > 0) {
                // spot light
                const float spotRangeDot = cosf(RADIAN_TO_DEGREE(lightMat.spotRange * 0.5f));
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
                
                // todo
//                const float glossy = interInfo.triangle->object.material.glossy;
//                
//                if (glossy > 0) {
//                    if (this->settings.shaderProvider < 5) {
//                        const vec3 r = reflect(-lightray, objectNormal).normalize();
//                        const float d = dot(r, (cameraWorldPos - hit).normalize());
//                        if (d > 0) {
//                            specluar = powf(d, 10000 * glossy);
//                        }
//                    } else {
//                        specluar = 0;
//                    }
//                }
                
                // final light color
                return clamp(lightMat.color * ((lum + specluar)));
            }
        }
    }

    return color3::zero;
}

color3 RayRenderer::traceLight(const vec3& hit, const vec3& normal) const {
    color3 areaLightColor, pointLightColor;

    const int areaLightSourceCount = (int)this->areaLightSources.size();
    const int pointLightSourceCount = (int)this->pointLightSources.size();

    if (areaLightSourceCount > 0) {
        const LightSource& ls = this->areaLightSources[rand() % areaLightSourceCount];
        areaLightColor = this->traceAreaLight(ls, hit, normal);
    }

    if (pointLightSourceCount > 0) {
        if (pointLightSourceCount == 1) {
            pointLightColor = this->tracePointLight(this->pointLightSources[0], hit, normal);
        }
        else {
            const LightSource& ls = this->pointLightSources[rand() % pointLightSourceCount];
            pointLightColor = this->tracePointLight(ls, hit, normal);
        }
    }

    return areaLightColor + pointLightColor;
}

//color3 RayRenderer::traceLight(const vec3& hit, const vec3& normal) const {
//    color3 areaLightColor, pointLightColor;
//
//    const int areaLightSourceCount = (int)this->areaLightSources.size();
//    const int pointLightSourceCount = (int)this->pointLightSources.size();
//
//    if (areaLightSourceCount > 0) {
//        const LightSource& ls = this->areaLightSources[rand() % areaLightSourceCount];
//        areaLightColor = this->traceAreaLight(ls, hit, normal);
//    }
//
//    if (pointLightSourceCount > 0) {
//        if (pointLightSourceCount == 1) {
//            pointLightColor = this->tracePointLight(this->pointLightSources[0], hit, normal);
//        }
//        else {
//            const LightSource& ls = this->pointLightSources[rand() % pointLightSourceCount];
//            pointLightColor = this->tracePointLight(ls, hit, normal);
//        }
//    }
//
//    return areaLightColor + pointLightColor;
//}

color3 RayRenderer::lambertTraceLights(const vec3& hit, const vec3& objectNormal) const {
    
    color3 areaLightColor, pointLightColor;

    const int areaLightSourceCount = (int)this->areaLightSources.size();

    if (areaLightSourceCount > 0) {
            const LightSource& ls = this->areaLightSources[rand() % areaLightSourceCount];
            areaLightColor += this->traceAreaLight(ls, hit, objectNormal);
    }

    for (const LightSource& ls : this->pointLightSources) {
        pointLightColor += this->tracePointLight(ls, hit, objectNormal);
    }
    
    return areaLightColor + pointLightColor + this->settings.worldColor;
}

void RayRenderer::calcVertexInterpolation(const RenderMeshTriangle& rt, const vec3& hit, VertexInterpolation* hi) const {
    const vec3 f1 = rt.v1 - hit;
    const vec3 f2 = rt.v2 - hit;
    const vec3 f3 = rt.v3 - hit;

//    const float a1 = fmaxf(cross(f2, f3).length() * rt.ti.a, 0);
//    const float a2 = fmaxf(cross(f3, f1).length() * rt.ti.a, 0);
//    const float a3 = fmaxf(cross(f1, f2).length() * rt.ti.a, 0);

    const float a1 = (cross(f2, f3).length() * rt.ti.a);
    const float a2 = (cross(f3, f1).length() * rt.ti.a);
    const float a3 = (cross(f1, f2).length() * rt.ti.a);

    hi->uv = rt.uv1 * a1 + rt.uv2 * a2 + rt.uv3 * a3;
    hi->normal = rt.n1 * a1 + rt.n2 * a2 + rt.n3 * a3;
}

void RayRenderer::calcVertexInterpolation(const RayTriangleIntersectionInfo& info, VertexInterpolation* vi) const {
    const auto* rt = info.triangle;
    
    // UV座標のバリセンター補間
    vi->uv = rt->uv1 * info.w + rt->uv2 * info.u + rt->uv3 * info.v;

    // 法線のバリセンター補間（正規化）
    vi->normal = (rt->n1 * info.w + rt->n2 * info.u + rt->n3 * info.v).normalize();
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

    float aoSum = 0.0f;
    
    for (int i = 0; i < this->settings.samples; i++) {
        
        const vec3 dir = cosineWeightedDirection(normal);
        Ray ray = ThicknessRay(vertex, dir);

        
        const bool occluded = this->bvh.intersectAny(ray, traceDistance, [](const RenderMeshTriangle* rt) {
            return rt->object.material.transparency < 0.01f;
        });

        if (!occluded) {
            float cosineTerm = fmaxf(dot(dir, normal), 0.0f); // Cosine-weight
            float distanceWeight = 1.0f; // 距離減衰は無し、または任意
            aoSum += cosineTerm * distanceWeight; // 遮蔽なしなら貢献
        }
    }
    
    // サンプル数で正規化
    float ao = aoSum / float(this->settings.samples);

    // AO値は [0,1] に制限（真っ白問題防止）
    //ao = powf(clamp(ao, 0.0f, 1.0f), 0.75f);

    return ao;
}

float RayRenderer::calcVertexAO(const Mesh& mesh, const int triangleIndex, const int vertexIndex, const float traceDistance) {
    auto& tr = this->meshTriangles[&mesh][triangleIndex];
    
    const vec3& v = tr->vs[vertexIndex];
    const vec3& n = tr->ns[vertexIndex];
    
    int s = 0;
    
    for (int i = 0; i < this->settings.samples; i++) {
        const vec3& dir = randomRayInHemisphere(n);
        
        Ray ray(v, dir);

        const bool blocked = this->bvh.intersectAny(ray, traceDistance, [](const RenderMeshTriangle*) {
            return true;
        });

        if (!blocked) {
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
                
        const auto* t = triangleList[ti];
        
        for (int i = 0; i < 3; i++) {
            const vec3& vertex = t->vs[i];
            const vec3 normal = t->ns[i];
            
            gray[i] = color3(.1, .1, .1) + this->traceLight(vertex, normal) * 0.9;
        }
        
        mesh.setColor(ti, gray[0], gray[1], gray[2]);
    }
}

inline bool RayRenderer::putTriangleIntoChildrenNode(RaySpaceTreeNode* node, const RenderMeshTriangle* rt) {
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

bool RayRenderer::putTriangleIntoTree(RaySpaceTreeNode* node, const RenderMeshTriangle* rt) {
    if (!node->splitted
        || !putTriangleIntoChildrenNode(node, rt))
    {
        node->list.push_back(rt);
    }

    return true;
}

inline bool rayIntersectTriangle3(const Ray& ray, const RenderMeshTriangle& rt, const float maxt, float& t, vec3& hit) {
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
void RayRenderer::scanBoundingBoxNearestTriangle(const Ray& ray, const RenderMeshTriangle* hitrt,
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


// Edge-avoiding À-Trous wavelet pass over rows [yStart, yEnd). The step size
// expands the 5×5 stencil each level (1, 2, 4, ...). Edge-stopping functions
// use luminance, encoded-normal cosine, and depth to gate the Gaussian kernel.
void RayRenderer::atrousPass(const Image3f& srcColor, Image3f& dstColor,
                             const Image3f& normal, const Image3f& depth,
                             int stepSize, int yStart, int yEnd) const {
    // B3 spline 5-tap: 1/16, 1/4, 3/8, 1/4, 1/16
    static const float kernel[5] = { 1.0f/16.0f, 1.0f/4.0f, 3.0f/8.0f, 1.0f/4.0f, 1.0f/16.0f };

    const int w = (int)srcColor.width();
    const int h = (int)srcColor.height();

    const float sigmaC = this->settings.denoiseSigmaColor;
    const float sigmaN = this->settings.denoiseSigmaNormal;
    const float sigmaD = this->settings.denoiseSigmaDepth;
    const float invSigmaC2 = 1.0f / (2.0f * sigmaC * sigmaC + 1e-8f);
    // Depth tolerance grows with step size so far-apart taps at high levels
    // don't erroneously reject over micro depth gradients.
    const float depthScale = 1.0f / (sigmaD * (float)stepSize + 1e-8f);

    for (int y = yStart; y < yEnd; ++y) {
        for (int x = 0; x < w; ++x) {
            const color4f cc = srcColor.getPixel(x, y);
            const color4f nc = normal.getPixel(x, y);
            const color4f dc = depth.getPixel(x, y);
            const vec3 centerColor(cc.r, cc.g, cc.b);
            const vec3 centerNormal(nc.r * 2.0f - 1.0f, nc.g * 2.0f - 1.0f, nc.b * 2.0f - 1.0f);
            const float centerDepth = dc.r;
            const float centerLum = 0.2126f * centerColor.x + 0.7152f * centerColor.y + 0.0722f * centerColor.z;

            vec3 sum(0.0f, 0.0f, 0.0f);
            float wsum = 0.0f;

            for (int ky = 0; ky < 5; ++ky) {
                const int ny = y + (ky - 2) * stepSize;
                if (ny < 0 || ny >= h) continue;
                for (int kx = 0; kx < 5; ++kx) {
                    const int nx = x + (kx - 2) * stepSize;
                    if (nx < 0 || nx >= w) continue;

                    const color4f cs = srcColor.getPixel(nx, ny);
                    const color4f ns = normal.getPixel(nx, ny);
                    const color4f ds = depth.getPixel(nx, ny);
                    const vec3 sampleColor(cs.r, cs.g, cs.b);
                    const vec3 sampleNormal(ns.r * 2.0f - 1.0f, ns.g * 2.0f - 1.0f, ns.b * 2.0f - 1.0f);
                    const float sampleDepth = ds.r;

                    const float sampleLum = 0.2126f * sampleColor.x + 0.7152f * sampleColor.y + 0.0722f * sampleColor.z;
                    const float lumDiff = sampleLum - centerLum;
                    const float wColor = expf(-(lumDiff * lumDiff) * invSigmaC2);

                    // Normal weight: cosine raised to σ_n; clamp dot to [0,1].
                    float ndot = sampleNormal.x * centerNormal.x
                               + sampleNormal.y * centerNormal.y
                               + sampleNormal.z * centerNormal.z;
                    if (ndot < 0.0f) ndot = 0.0f;
                    const float wNormal = powf(ndot, sigmaN);

                    const float depthDiff = fabsf(sampleDepth - centerDepth);
                    const float wDepth = expf(-depthDiff * depthScale);

                    const float wKernel = kernel[kx] * kernel[ky];
                    const float weight = wKernel * wColor * wNormal * wDepth;

                    sum.x += sampleColor.x * weight;
                    sum.y += sampleColor.y * weight;
                    sum.z += sampleColor.z * weight;
                    wsum += weight;
                }
            }

            const vec3 finalColor = (wsum > 1e-8f) ? (sum / wsum) : centerColor;
            dstColor.setPixel(x, y, color4(finalColor, cc.a));
        }
    }
}

void RayRenderer::applyTonemapGamma(Image& img) const {
    const int w = (int)img.width();
    const int h = (int)img.height();
    const float invGamma = 1.0f / 2.2f;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const color4f c = img.getPixel(x, y);
            // Luminance-based Reinhard (see renderPixel for the rationale).
            const float L = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
            float mr = 0.0f, mg = 0.0f, mb = 0.0f;
            if (L > 1e-6f) {
                const float Lmapped = L / (L + 1.0f);
                const float scale = Lmapped / L;
                mr = c.r * scale;
                mg = c.g * scale;
                mb = c.b * scale;
                const float peak = fmaxf(fmaxf(mr, mg), mb);
                if (peak > 1.0f) {
                    const float inv = 1.0f / peak;
                    mr *= inv; mg *= inv; mb *= inv;
                }
            }
            color4f out(powf(fmaxf(mr, 0.0f), invGamma),
                        powf(fmaxf(mg, 0.0f), invGamma),
                        powf(fmaxf(mb, 0.0f), invGamma),
                        c.a);
            if (out.r > 1.0f) out.r = 1.0f;
            if (out.g > 1.0f) out.g = 1.0f;
            if (out.b > 1.0f) out.b = 1.0f;
            img.setPixel(x, y, out);
        }
    }
}

void RayRenderer::denoiseImage(const Image3f& noisy, const Image3f& normal,
                               const Image3f& depth, const Image3f& albedo,
                               Image3f& output) {
    const int w = (int)noisy.width();
    const int h = (int)noisy.height();
    const int levels = std::max(1, this->settings.denoiseLevels);
    const int numThreads = std::max(1, this->settings.threads);

    Image3f bufA, bufB;
    bufA.createEmpty(w, h);
    bufB.createEmpty(w, h);

    // Pre-pass: firefly suppression + albedo demodulation.
    //
    // Fireflies — isolated bright pixels from rare high-contribution paths
    // (e.g. a lucky NEE hit on a dark material) — are preserved by the
    // edge-stopping kernel because their color differs so much from
    // neighbors that the weight collapses to zero. At samples=1 this shows
    // up as persistent bright dots. A soft clamp against the 3×3 local max
    // luminance pulls them into a reasonable range before filtering without
    // touching legitimate bright regions (where the max is already close).
    //
    // Demodulation then divides the suppressed noisy signal by albedo so
    // the filter operates on "incoming lighting" — smooth across material
    // boundaries — and we re-multiply by albedo after the final pass to
    // restore color detail. The albedo floor prevents division-blowup on
    // dark channels; the demod cap bounds any residual amplification.
    // AOV alpha channel flags geometry hits (1) vs sky/miss (0); sky
    // pixels pass through unchanged.
    const float albedoFloor = 0.3f;
    const float demodCap = 3.0f;
    const float fireflyRatio = 1.5f;
    const float fireflyEps = 0.01f;
    auto luminance = [](float r, float g, float b) {
        return 0.2126f * r + 0.7152f * g + 0.0722f * b;
    };
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            color4f c = noisy.getPixel(x, y);

            float maxNeighLum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy) {
                const int ny = y + dy;
                if (ny < 0 || ny >= h) continue;
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx == 0 && dy == 0) continue;
                    const int nx = x + dx;
                    if (nx < 0 || nx >= w) continue;
                    const color4f n = noisy.getPixel(nx, ny);
                    const float nlum = luminance(n.r, n.g, n.b);
                    if (nlum > maxNeighLum) maxNeighLum = nlum;
                }
            }
            const float centerLum = luminance(c.r, c.g, c.b);
            const float cap = fireflyRatio * maxNeighLum + fireflyEps;
            if (centerLum > cap) {
                const float scale = cap / centerLum;
                c.r *= scale; c.g *= scale; c.b *= scale;
            }

            const color4f a = albedo.getPixel(x, y);
            if (a.a > 0.5f) {
                const float ar = fmaxf(a.r, albedoFloor);
                const float ag = fmaxf(a.g, albedoFloor);
                const float ab = fmaxf(a.b, albedoFloor);
                bufA.setPixel(x, y, color4f(fminf(c.r / ar, demodCap),
                                            fminf(c.g / ag, demodCap),
                                            fminf(c.b / ab, demodCap),
                                            1.0f));
            } else {
                bufA.setPixel(x, y, c);
            }
        }
    }

    Image3f* src = &bufA;
    Image3f* dst = &bufB;

    for (int level = 0; level < levels; ++level) {
        const int stepSize = 1 << level;

        std::vector<std::thread> workers;
        workers.reserve(numThreads);
        const int rowsPerThread = (h + numThreads - 1) / numThreads;
        for (int t = 0; t < numThreads; ++t) {
            const int yStart = t * rowsPerThread;
            const int yEnd = std::min(h, yStart + rowsPerThread);
            if (yStart >= yEnd) break;
            const Image3f* srcConst = src;
            Image3f* dstPtr = dst;
            workers.emplace_back([this, srcConst, dstPtr, &normal, &depth, stepSize, yStart, yEnd] {
                this->atrousPass(*srcConst, *dstPtr, normal, depth, stepSize, yStart, yEnd);
            });
        }
        for (std::thread& th : workers) th.join();

        Image3f* tmp = src; src = dst; dst = tmp;
    }

    // Remodulate and blend with the original noisy input by `denoiseIntensity`.
    // intensity = 1 → pure filtered output (full denoise)
    // intensity = 0 → original noisy (pass-through, effectively disabled)
    // Blending happens in linear-HDR space, before the post-denoise tonemap.
    const Image3f& filtered = *src;
    const float t = clamp(this->settings.denoiseIntensity, 0.0f, 1.0f);
    const float s = 1.0f - t;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const color4f f = filtered.getPixel(x, y);
            const color4f n = noisy.getPixel(x, y);
            const color4f a = albedo.getPixel(x, y);
            color4f remod;
            if (a.a > 0.5f) {
                remod = color4f(f.r * a.r, f.g * a.g, f.b * a.b, 1.0f);
            } else {
                remod = f;
            }
            output.setPixel(x, y, color4f(n.r * s + remod.r * t,
                                          n.g * s + remod.g * t,
                                          n.b * s + remod.b * t,
                                          1.0f));
        }
    }
}

//--------------------------------------

color3 RayBSDFShaderProvider::shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray,
                                    const VertexInterpolation& vi, void* shaderParam) {
    const Material& m = interInfo.triangle->object.material;
    BSDFParam param(*this->renderer, interInfo, inray, vi);

    if (m.emission > 0.0f) {
        const color3 emission = m.color * m.emission;

        // MIS partner for traceAreaLight: if the caller sampled this direction
        // from a cos-weighted diffuse lobe, weight the emission we found with
        // the BSDF-strategy power-heuristic weight. Otherwise (eye ray, or a
        // non-MIS caller like GlossyShader), return the full emission.
        if (shaderParam != NULL) {
            const BSDFParam* sp = (const BSDFParam*)shaderParam;
            if (sp->misDiffuse) {
                const float cosObj = dot(sp->misNormal, inray.dir);
                const float cosLight = -dot(inray.dir, vi.normal);
                if (cosObj <= 0.0f || cosLight <= 0.0f) return color3::zero;

                const float r2 = (interInfo.hit - inray.origin).length2();
                const float sampledArea = this->renderer->areaLightSampledArea(*interInfo.triangle);
                const float pdfLight = r2 / (cosLight * sampledArea);
                const float pdfBsdf = cosObj / (float)M_PI;
                const float pdfLight2 = pdfLight * pdfLight;
                const float pdfBsdf2 = pdfBsdf * pdfBsdf;
                const float wBsdf = pdfBsdf2 / (pdfBsdf2 + pdfLight2);
                return emission * wBsdf;
            }
        }
        return emission;
    }

    if (dot(inray.dir, vi.normal) > 0.0f) {
        if (m.transparency > 0.001f) {
            const BSDFParam* sp = (const BSDFParam*)shaderParam;
            if (sp != NULL && sp->passes + 1 <= MAX_TRACE_DEPTH) {
                param.passes = sp->passes + 1;
                param.throughput = sp->throughput;
                return transparencyShader.shade(param);
            } else {
                return color3::zero;
            }
        }
        else if (m.refraction < 0.001f && m.glossy > 0.001f) {
            return color3::zero;
        }
    }

    if (shaderParam != NULL) {
        const BSDFParam* sp = (const BSDFParam*)shaderParam;

        if (sp->passes + 1 >= MAX_TRACE_DEPTH) {
            if (1.0f - m.glossy - m.refraction > 0.00001f) {
                const color3 light = this->renderer->traceLight(interInfo.hit, vi.normal);

                color3 color = color3::zero;
                if (this->renderer->settings.enableColorSampling) {
                    color = m.color;

                    if (m.texture != NULL) {
                        color *= m.texture->sample(vi.uv * m.texTiling).rgb;
                    }
                }

                return light * color;
            } else {
                return color3::zero;
            }
        }

        param.passes = sp->passes + 1;
        param.throughput = sp->throughput;
        // Propagate the chromatic-dispersion channel so every refractive
        // interface on this path uses the same wavelength — otherwise the
        // per-interface channel masks zero each other out.
        param.chromaChannel = sp->chromaChannel;
        // Carry the envmap-MIS BSDF pdf forward too. It is relevant only at
        // the immediate next hit (where tracePath would consume it); after
        // any further bounce through a BSDF, the direction is re-sampled and
        // this pdf no longer applies. Shaders that resample should clear it.
        param.misEnvBsdfPdf = sp->misEnvBsdfPdf;

        // Russian Roulette: kill low-contribution paths probabilistically and
        // rescale survivors by 1/q to keep the estimator unbiased. The max
        // channel of the accumulated throughput is a cheap, reasonable q:
        // bright paths survive, dim paths die off quickly.
        float rrWeight = 1.0f;
        if (param.passes >= MIN_RR_DEPTH) {
            const color3& t = param.throughput;
            float q = fmaxf(t.r, fmaxf(t.g, t.b));
            q = fminf(RR_MAX_PROB, fmaxf(RR_MIN_PROB, q));
            if (randomValue() >= q) return color3::zero;
            rrWeight = 1.0f / q;
        }

        if (m.transparency > 0.001f) {
            return transparencyShader.shade(param) * rrWeight;
        } else {
            return mixShader.shade(param) * rrWeight;
        }
    } else {
        if (m.transparency > 0.01f) {
            return mixShader.shade(param) * (1.0f - m.transparency) + transparencyShader.shade(param);
        } else {
            return mixShader.shade(param);
        }
    }
}

color3 RayBSDFBakeShaderProvider::shade(const RayTriangleIntersectionInfo& interInfo, const Ray& inray,
                                        const VertexInterpolation& vi, void* shaderParam) {
    const Material& m = interInfo.triangle->object.material;
    
    if (m.emission > 0.0f) {
        return (m.color * m.emission);
    }

    BSDFParam param(*this->renderer, interInfo, inray, vi);

    if (m.transparency > 0.01f) {
        return this->transparencyShader.shade(param);
    }
    
    if (shaderParam != NULL) {

        BSDFParam* sp = (BSDFParam*)shaderParam;
        
        if (sp->passes >= 2) {
            return this->renderer->traceLight(interInfo.hit, vi.normal) * m.color;
        }

        param.passes = sp->passes + 1;
    }
    
#ifdef CUT_OFF_BACK_TRACE
    if (m.transparency <= 0.001f && dot(-inray.dir, vi.normal) <= 0.0f) {
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

