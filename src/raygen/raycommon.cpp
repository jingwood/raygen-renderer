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

void RayRenderTriangle::precalc() {
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

bool RayRenderTriangle::intersectsRay(const Ray& ray, float maxt, float& t, vec3& hit) const {
	
//	if (dot(this->faceNormal, normalize(ray.dir)) < 0) {
//		return false;
//	}
	
	const float dist = -dot(this->ti.l, vec4(ray.origin, 1.0f)) / dot(this->ti.l, vec4(ray.dir, 0.0f));

	if (dist < 0 || std::isnan(dist) || dist > maxt) {
		return false;
	}

	t = dist;
	hit = ray.origin + ray.dir * dist;

	vec3 c;

	c = cross(this->v2 - this->v1, hit - this->v1);
	if (dot(this->ti.pd, c) < 0) return false;

	c = cross(this->v3 - this->v2, hit - this->v2);
	if (dot(this->ti.pd, c) < 0) return false;

	c = cross(this->v1 - this->v3, hit - this->v3);
	if (dot(this->ti.pd, c) < 0) return false;

	return true;
}

bool RayRenderTriangle::containsUVPoint(const vec2& uv) const {

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