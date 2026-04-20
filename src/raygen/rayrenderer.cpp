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

    // Pinhole camera: fieldOfView is vertical FoV in degrees.
    // Clamp to < 180° since tan(90°) is undefined; use 179° as a safe upper bound.
    const float fovDeg = fminf(fmaxf(camera->fieldOfView, 1.0f), 179.0f);
    const float tanHalfFov = tanf(DEGREE_TO_RADIAN(fovDeg) * 0.5f);

    // dx/dy are angular offsets (tangent-space): at depth z, world offset = (dx*z, dy*z).
    ctx->viewScaleY = (2.0f * tanHalfFov) / ctx->renderSize.height;
    ctx->viewScaleX = ctx->viewScaleY * ctx->aspectRate;

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
    
    this->normalBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
    this->depthBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
    this->albedoBuffer.createEmpty(ctx.renderSize.width, ctx.renderSize.height);
    
    this->progressRate = 0;

    std::vector<std::thread> threads;
    
    for (int i = 0; i < this->settings.threads; i++) {
        threads.push_back(std::thread([this, ctx, i] { this->renderThread(ctx, i); }));
    }
    
    for (std::thread &th : threads) {
        th.join();
    }
    
    if (this->settings.enableRenderingPostProcess) {
        Image3f denoised = this->denoiseImage(this->renderingImage, this->normalBuffer, this->depthBuffer, this->albedoBuffer);
        Image::copy(denoised, this->renderingImage);

        Image glowimg(this->renderingImage.getPixelDataFormat(), 32);
        Image::copy(this->renderingImage, glowimg);
        glowimg.resize((int)((float)this->renderingImage.width() * PP_GLOW_SIZE_ASPECT),
            (int)((float)this->renderingImage.height() * PP_GLOW_SIZE_ASPECT));
        img::thresholdSoft(glowimg, 0.9f, 3);
        img::gamma(glowimg, PP_GLOW_GAMMA);
        int kernelSize = calculateGaussianKernelSize(glowimg.width(), glowimg.height());
        img::gaussBlur(glowimg, kernelSize);
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

color4f RayRenderer::renderPixel(const RenderThreadContext& ctx, Ray& ray, const int x, const int y) {

    // Guided Denoise Meta Data - Start
    ViewRaySurfaceInfo traceRayInfo;
    this->traceEyeRaySurfaceInfo(ray, &traceRayInfo);
    
    // 法線バッファ（-1〜1 → 0〜1にマッピングして保存）
    vec3 normalColor = traceRayInfo.hi.normal * 0.5f + 0.5f;
    this->normalBuffer.setPixel(x, y, color4(normalColor, 1.0f));

    // アルベド（diffuse color）
    this->albedoBuffer.setPixel(x, y, traceRayInfo.mat == NULL ? this->settings.backColor :  color4(traceRayInfo.mat->color, 1)); // or texture sampled color

    // 深度（距離をグレースケール化）
    float distance = (traceRayInfo.interInfo.hit - cameraWorldPos).length();
    float depth = distance / scene->mainCamera->viewFar;
    depth = sqrt(depth);
    depth = 1.0 - clamp(depth, 0.0f, 1.0f);
    this->depthBuffer.setPixel(x, y, vec3(depth));
    // Guided Denoise Meta Data - End


    // Angular offsets (tangent-space). Ray through pixel is normalize(vec3(dx, dy, -1)).
    const float dx = ((float)x + 0.5f - ctx.halfRenderSize.width) * ctx.viewScaleX;
    const float dy = -((float)y + 0.5f - ctx.halfRenderSize.height) * ctx.viewScaleY;

    color4f sampleColor;
    const int totalSamples = this->settings.samples;

    for (int i = 0; i < totalSamples; i++) {
        // Reset the Halton walk for this (pixel, sample). The early dims (0,1
        // for sub-pixel jitter, 2,3 for DOF) are the best-stratified slots, and
        // the remaining dims propagate down into the path trace for BSDF /
        // light sampling.
        ldsBeginPixelSample(x, y, i);

        if (ctx.depthOfField >= 0.001f && ctx.aperture > 0.0f) {
            // Focal point at depth `depthOfField` along the primary ray direction.
            const vec3 focalPoint(dx * ctx.depthOfField, dy * ctx.depthOfField, -ctx.depthOfField);

            // Sub-pixel jitter on dims 0,1 so pixel coverage is stratified.
            float jx, jy;
            ldsNext2D(jx, jy);
            const float pxDx = dx + (jx - 0.5f) * ctx.viewScaleX;
            const float pxDy = dy - (jy - 0.5f) * ctx.viewScaleY;
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
                const float r = sqrtf(du);
                const float theta = dv * 2.0f * (float)M_PI;
                offsetX = r * cosf(theta) * ctx.aperture;
                offsetY = r * sinf(theta) * ctx.aperture;
            }

            ray.origin = vec3(offsetX, offsetY, 0.0f);
            ray.dir = (focalPointJ - ray.origin).normalize();
        } else {
            // Sub-pixel jitter for stochastic anti-aliasing; ray direction pivots at origin.
            float jx, jy;
            ldsNext2D(jx, jy);
            ray.origin = vec3::zero;
            ray.dir = vec3(dx + (jx - 0.5f) * ctx.viewScaleX,
                           dy - (jy - 0.5f) * ctx.viewScaleY,
                           -1.0f).normalize();
        }

        sampleColor += this->traceEyeRay(ray);
    }

    const color3f radiance = sampleColor * ctx.exposure / (float)totalSamples;
    // Reinhard tone map so highlights > 1.0 compress instead of clipping to
    // white (the hard clamp was flattening wood grain / checker patterns
    // into a solid 1.0 under bright lights). Per-channel x/(x+1) keeps darks
    // linear and asymptotes at 1.0. Then approximate sRGB gamma (1/2.2) so
    // linear mid-grey renders as expected on a display-referred JPG viewer —
    // without this, output looks crushed and dark regardless of radiance.
    const color3f mapped(radiance.r / (radiance.r + 1.0f),
                         radiance.g / (radiance.g + 1.0f),
                         radiance.b / (radiance.b + 1.0f));
    const float invGamma = 1.0f / 2.2f;
    const color3f encoded(powf(fmaxf(mapped.r, 0.0f), invGamma),
                          powf(fmaxf(mapped.g, 0.0f), invGamma),
                          powf(fmaxf(mapped.b, 0.0f), invGamma));
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
            return clamp(this->shaderProvider->shade(interInfo, ray, vi), 0.0f, 1.0f);
        }
    }

    const color3 env = this->sampleEnvironment(ray.dir);
    if (env != color3::zero) {
        return color4(env, 1.0f);
    }
    return this->settings.backColor;
}

color3 RayRenderer::sampleEnvironment(const vec3& dir) const {
    if (this->scene == NULL || this->scene->envmap == NULL) return color3::zero;

    const vec3 d = dir.normalize();
    const float yaw = this->scene->envmapRotation * (float)(M_PI / 180.0);
    const float cosY = cosf(yaw), sinY = sinf(yaw);
    // Rotate the sample direction around Y before looking it up so the map
    // can be oriented without touching the texture.
    const float rx = d.x * cosY - d.z * sinY;
    const float rz = d.x * sinY + d.z * cosY;

    // Equirectangular (lat-long) mapping: u from azimuth, v from elevation.
    const float u = 0.5f + atan2f(rz, rx) * (float)(0.5 / M_PI);
    const float v = 0.5f - asinf(clamp(d.y, -1.0f, 1.0f)) * (float)(1.0 / M_PI);

    const color3 sample = this->scene->envmap->sample(vec2(u, v)).rgb;
    return sample * this->scene->envmapIntensity;
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
    // from every direction. Falls back to zero when no envmap is set, so the
    // estimator behaves the same as before for envmap-less scenes.
    return this->sampleEnvironment(ray.dir);
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


Image3f RayRenderer::denoiseImage(const Image3f& noisy, const Image3f& normal, const Image3f& depth, const Image3f& albedo) {
  Image3f denoised;
  denoised.createEmpty(noisy.width(), noisy.height());

  const int radius = 2;
  const float sigmaColor = 0.6f;
  const float sigmaNormal = 0.4f;
  const float sigmaDepth = 0.2f;

  for (int y = 0; y < noisy.height(); ++y) {
    for (int x = 0; x < noisy.width(); ++x) {
      const vec3 centerColor = noisy.getPixel(x, y).rgb;
      const vec3 centerNormal = normal.getPixel(x, y).rgb * 2.0f - 1.0f;
      const float centerDepth = depth.getPixel(x, y).r;

      vec3 sum = vec3(0);
      float totalWeight = 0.0f;

      for (int dy = -radius; dy <= radius; ++dy) {
        for (int dx = -radius; dx <= radius; ++dx) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= noisy.width() || ny >= noisy.height()) continue;

          const vec3 sampleColor = noisy.getPixel(nx, ny).rgb;
          const vec3 sampleNormal = normal.getPixel(nx, ny).rgb * 2.0f - 1.0f;
          const float sampleDepth = depth.getPixel(nx, ny).r;

          float wColor = expf(-(sampleColor - centerColor).length2() / (2 * sigmaColor * sigmaColor));
          float wNormal = expf(-(sampleNormal - centerNormal).length2() / (2 * sigmaNormal * sigmaNormal));
          float wDepth = expf(-powf(sampleDepth - centerDepth, 2.0f) / (2 * sigmaDepth * sigmaDepth));

          float weight = wColor * wNormal * wDepth;

          sum += sampleColor * weight;
          totalWeight += weight;
        }
      }

      vec3 finalColor = (totalWeight > 0) ? sum / totalWeight : centerColor;
      denoised.setPixel(x, y, color4(finalColor, 1.0f));
    }
  }

  return denoised;
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

