///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __RAY_COMMON_H__
#define __RAY_COMMON_H__

#include "ugm/types3d.h"
#include "scene.h"

#define SURFACE_THICKNESS 0.00001f
#define MAX_RAY_DISTANCE 999.9f

namespace raygen {

inline vec3 reflect(const vec3& d, const vec3& normal) {
	return d - normal * (dot(d, normal) * 2.0f);
}

//inline vec3 refract(const vec3& d, const vec3& normal, float r = 1.45) {
//	const vec3 nl = dot(d, normal) < 0 ? normal : -normal;
//	const bool into = dot(nl, normal) > 0;
//	if (into) r = 1.0f / r;
//	
//	const float c = dot(d, nl);
//	const float t = 1.0f - r * r * (1.0f - c * c);
//	
//	if (t < 0) {
//		return reflect(d, normal);
//	}
//	
//	//	return d * r - normal * (r * c + sqrtf(t));
//	return normalize(d * r - normal * ((into ? 1 : -1) * (c * r + sqrt(t))));
//}

vec3 cosineWeightedDirection(const vec3& normal);
vec3 ldsPointInTriangle(const Triangle& tri);

// Low-discrepancy sampler: thread-local state; each consumer pulls the next
// Halton dimension and advances. Call ldsBeginPixelSample at the top of each
// pixel-sample loop so the first few dims get stratified across samples; at
// high dims we fall back to the PRNG, which is fine for tail bounces.
void ldsBeginPixelSample(int x, int y, int sampleIdx);
float ldsNext1D();
void ldsNext2D(float& u, float& v);

inline Ray ThicknessRay(const vec3& origin, const vec3& dir) {
	return Ray(origin + dir.normalize() * SURFACE_THICKNESS, dir);
}

struct RayTriangleIntersectionInfo;

class RenderMeshTriangle {
public:
	union {
		struct {
			union {
				struct { vec3 v1, v2, v3; };
				vec3 vs[3];
			};
			union {
				struct { vec3 n1, n2, n3; };
				vec3 ns[3];
			};
			union {
				union {
					struct { vec2 uv1, uv2, uv3, uv4, uv5, uv6; };
					struct {
						struct {
							vec2 uv1, uv2, uv3;
						} uvset1;
						struct {
							vec2 uv1, uv2, uv3;
						} uvset2;
					};
				};
				vec2 uvs[6];
			};
		};
		TriangleNUV tnuv;
		Triangle tri;
	};

	struct {
		float a;
		vec3 pd;
		vec3 normalizedpd;
		vec4 l;
		float pdlen;
	} ti;

	struct {
		vec2 mp;
		float a, b, c, d, e, f;
		float area;
//		struct {
//			bool e1, e2, e3;
//		} shared;
		BBox2D box;
	} uvt2Info;
	
	vec3 faceNormal;
    float area;
    float pdf;

	BoundingBox bbox;

	const SceneObject& object;
	const Mesh& mesh;

	RenderMeshTriangle(const vec3& v1, const vec3& v2, const vec3& v3,
		const vec3& n1, const vec3& n2, const vec3& n3,
		const vec2& uv1, const vec2& uv2, const vec2& uv3,
		const vec2& uv4, const vec2& uv5, const vec2& uv6,
		const SceneObject& obj, const Mesh& mesh)
		: v1(v1), v2(v2), v3(v3), n1(n1), n2(n2), n3(n3),
			uv1(uv1), uv2(uv2), uv3(uv3), uv4(uv4), uv5(uv5), uv6(uv6),
			object(obj), mesh(mesh)
	{
		this->precalc();
		this->faceNormal = (n1 + n2 + n3) / 3.0f;
		this->bbox = BoundingBox::fromTriangle(v1, v2, v3);
	}

	void precalc();
	bool intersectsRay(const Ray& ray, float maxt, float& t, vec3& hit) const;
    bool intersectsRay(const Ray& ray, RayTriangleIntersectionInfo& interInfo) const;
    
	bool containsUVPoint(const vec2& uv) const;
};

struct RayTriangleIntersectionInfo {
    const RenderMeshTriangle* triangle;
    float t;        // disance
    vec3 hit;       // hit point
    float u, v, w;  // hit interpolation middle info
    
    RayTriangleIntersectionInfo(const RenderMeshTriangle* triangle = NULL, float t = MAX_RAY_DISTANCE,
                                vec3 hit = vec3::zero, float u = 0, float v = 0, float w = 0)
        : triangle(triangle), t(t), hit(hit), u(u), v(v), w(w)
    {
        
    }
};

struct RayMeshIntersection {
	const RenderMeshTriangle* rt;
	float t;
	vec3 hit;

	RayMeshIntersection(const RenderMeshTriangle* rt = NULL, const float t = 0, const vec3& hit = vec3::zero)
		: rt(rt), t(t), hit(hit)
	{ }
};

struct VertexInterpolation
{
	vec3 normal;
	vec2 uv;
};

struct TracePath
{
	const RenderMeshTriangle* fromRt;
	Ray fromRay;
	const RenderMeshTriangle* hitRt;
  color3f hitColor;
};

}

#endif /* __RAY_COMMON_H__ */
