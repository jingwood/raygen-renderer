///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef polygons3d_h
#define polygons3d_h

#include <stdio.h>
#include "mesh.h"

namespace raygen {

class PlaneMesh : public Mesh {
public:
	PlaneMesh(const int w = 1, const int h = 1);
	PlaneMesh(const vec2& from, const vec2& to);
	PlaneMesh(const vec3& from, const vec3& to);
	
private:
	void create(const vec3& from, const vec3& to);
};

class CubeMesh : public Mesh {
public:
	CubeMesh();
};

class SphereMesh: public Mesh {
public:
    SphereMesh(float radius = 1, int stacks = 16, int slices = 32);
};

// Truncated cone (frustum) along the Z axis, centered at origin from
// z = -0.5 to z = +0.5 so it composes with `scale: [w, h, length]` the
// same way CubeMesh does. radiusStart is the radius at z = +0.5 (the
// object-local "forward" disk), radiusEnd is the radius at z = -0.5
// (the rearward disk). Either radius may be 0 for a true cone tip;
// in that case the corresponding cap is omitted.
class ConeMesh : public Mesh {
public:
    ConeMesh(float radiusStart = 0.5f, float radiusEnd = 0.5f, int slices = 32);
};

}

#endif /* polygons3d_h */
