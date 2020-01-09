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
	PlaneMesh();
	PlaneMesh(const int w, const int h);
	PlaneMesh(const vec2& from, const vec2& to);
	PlaneMesh(const vec3& from, const vec3& to);
	
	void create(const vec3& from, const vec3& to);
};

class CubeMesh : public Mesh {
public:
	CubeMesh();
};

}

#endif /* polygons3d_h */
