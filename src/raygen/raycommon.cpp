///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "raycommon.h"
#include <cmath>

namespace raygen {

vec3 cosineWeightedDirection(const vec3& normal) {
    float r1 = randomValue();
    float r2 = randomValue();

    float theta = acos(sqrt(1.0f - r1));
    float phi = 2.0f * M_PI * r2;

    float x = sin(theta) * cos(phi);
    float y = sin(theta) * sin(phi);
    float z = cos(theta);

    // 局所座標からグローバルへ変換
    vec3 tangent;
    if (fabs(normal.x) > fabs(normal.y)) {
        tangent = normalize(cross(vec3(0.0f, 1.0f, 0.0f), normal));
    } else {
        tangent = normalize(cross(vec3(1.0f, 0.0f, 0.0f), normal));
    }
    vec3 bitangent = cross(normal, tangent);

    return normalize(tangent * x + bitangent * y + normal * z);
}

void RenderMeshTriangle::precalc() {
	vec3 pd = cross(this->v2 - this->v1, this->v3 - this->v2);

	this->ti.a = 1.0f / cross(this->v1 - this->v2, this->v1 - this->v3).length();
	this->ti.pd = pd;
	this->ti.pdlen = pd.length();
	this->ti.normalizedpd = pd.normalize();
	this->ti.l = vec4(pd.x, pd.y, pd.z, dot(-pd, this->v1)) * (1.0f / this->ti.pdlen);
	
	this->uvt2Info.mp = (uv4 + uv5 + uv6) / 3.0;
	this->uvt2Info.box = BBox2D::fromTriangle(this->uv4, this->uv5, this->uv6);
	this->uvt2Info.a = uv4.y * uv6.x - uv4.x * uv6.y;
	this->uvt2Info.b = uv6.y - uv4.y;
	this->uvt2Info.c = uv4.x - uv6.x;
	this->uvt2Info.d = uv4.x * uv5.y - uv4.y * uv5.x;
	this->uvt2Info.e = uv4.y - uv5.y;
	this->uvt2Info.f = uv5.x - uv4.x;
	this->uvt2Info.area = -uv5.y * uv6.x + uv4.y * (uv6.x - uv5.x) + uv4.x * (uv5.y - uv6.y) + uv5.x * uv6.y;
}

bool RenderMeshTriangle::intersectsRay(const Ray& ray, float maxt, float& t, vec3& hit) const {
//	const float dist = -dot(this->ti.l, vec4(ray.origin, 1.0f)) / dot(this->ti.l, vec4(ray.dir, 0.0f));
//
//	if (dist < 0 || std::isnan(dist) || dist > maxt) {
//		return false;
//	}
//
//	t = dist;
//	hit = ray.origin + ray.dir * dist;
//
//	vec3 c;
//
//	c = cross(this->v2 - this->v1, hit - this->v1);
//	if (dot(this->ti.pd, c) < 0) return false;
//
//	c = cross(this->v3 - this->v2, hit - this->v2);
//	if (dot(this->ti.pd, c) < 0) return false;
//
//	c = cross(this->v1 - this->v3, hit - this->v3);
//	if (dot(this->ti.pd, c) < 0) return false;
//
//	return true;
    const float EPSILON = 1e-6f;
    vec3 edge1 = v2 - v1;
    vec3 edge2 = v3 - v1;
    vec3 h = cross(ray.dir, edge2);
    float a = dot(edge1, h);

    if (fabs(a) < EPSILON) return false;  // レイは三角形と平行

    float f = 1.0f / a;
    vec3 s = ray.origin - v1;
    float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f) return false;

    vec3 q = cross(s, edge1);
    float v = f * dot(ray.dir, q);
    if (v < 0.0f || u + v > 1.0f) return false;

    float tempT = f * dot(edge2, q);
    if (tempT > EPSILON && tempT < maxt) {
        t = tempT;
        hit = ray.origin + ray.dir * t;
        return true;
    }

    return false;
}

bool RenderMeshTriangle::intersectsRay(const Ray& ray, RayTriangleIntersectionInfo& info) const {
    // 三角形のエッジ計算
    vec3 edge1 = v2 - v1;
    vec3 edge2 = v3 - v1;
    vec3 pvec = cross(ray.dir, edge2);
    float det = dot(edge1, pvec);

    if (fabs(det) < 1e-8f) return false;

    float invDet = 1.0f / det;
    vec3 tvec = ray.origin - v1;
    float u = dot(tvec, pvec) * invDet;
    if (u < 0.0f || u > 1.0f) return false;

    vec3 qvec = cross(tvec, edge1);
    float v = dot(ray.dir, qvec) * invDet;
    if (v < 0.0f || u + v > 1.0f) return false;

    float t = dot(edge2, qvec) * invDet;
    if (t < 0.0f)
    {
        return false;
    }
    if (t >= info.t) {
        return false;  // すでにもっと近いものがある
    }
    
    info.t = t;
    info.hit = ray.origin + ray.dir * t;
    info.u = u;
    info.v = v;
    info.w = 1.0f - u - v;
    info.triangle = this;
    
    return true;
}


bool RenderMeshTriangle::containsUVPoint(const vec2& uv) const {

//	if (!this->uvt2Info.box.contains(uv)) {
//		return false;
//	}
	
	float s = uvt2Info.a + uvt2Info.b * uv.x + uvt2Info.c * uv.y;
	float t = uvt2Info.d + uvt2Info.e * uv.x + uvt2Info.f * uv.y;
	
	if ((s < 0) != (t < 0))
		return false;
	
	float area = uvt2Info.area;
	
	if (area < 0.0) {
		s = -s;
		t = -t;
		area = -area;
	}
	
	return s > 0 && t > 0 && (s + t) <= area;
}

}
