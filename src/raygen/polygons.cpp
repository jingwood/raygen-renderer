///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "polygons.h"

namespace raygen {

PlaneMesh::PlaneMesh(const int w, const int h) {
	const float halfw = w * 0.5;
	const float halfh = h * 0.5;

	this->create(vec3(-halfw, 0, -halfh), vec3(halfw, 0, halfh));
}

PlaneMesh::PlaneMesh(const vec2& from, const vec2& to)
: PlaneMesh(vec3(from.x, 0, from.y), vec3(to.x, 0, to.y)) {
}

PlaneMesh::PlaneMesh(const vec3& from, const vec3& to)
: PlaneMesh() {
	this->create(from, to);
}

void PlaneMesh::create(const vec3& from, const vec3& to) {
	this->hasNormal = true;
	this->hasTexcoord = true;
	
	this->init(6);

	// TODO: may need to fix y
	
	this->vertices[0] = vec3(from.x, from.y, from.z);
	this->vertices[1] = vec3(from.x, from.y, to.z);
	this->vertices[2] = vec3(to.x, to.y, from.z);
	
	this->vertices[3] = vec3(from.x, from.y, to.z);
	this->vertices[4] = vec3(to.x, to.y, to.z);
	this->vertices[5] = vec3(to.x, to.y, from.z);
	
	for (int i = 0; i < 6; i++) {
		this->normals[i] = vec3::up;
	}
	
	this->texcoords[0] = vec2(0, 0);
	this->texcoords[1] = vec2(0, 1);
	this->texcoords[2] = vec2(1, 0);
	
	this->texcoords[3] = vec2(0, 1);
	this->texcoords[4] = vec2(1, 1);
	this->texcoords[5] = vec2(1, 0);
	
	this->calcTangentBasis();
	this->calcBoundingBox();

}

static constexpr float cubeVertexBuffer[] = {
	0.5, -0.5, 0.5, -0.5, -0.5, 0.5, -0.5, -0.5, -0.5, -0.5, 0.5, -0.5,
	-0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, -0.5, 0.5, 0.5, 0.5, 0.5, -0.5, 0.5, 0.5, 0.5, 0.5, -0.5, 0.5, 0.5,
	-0.5, -0.5, 0.5, -0.5, 0.5, 0.5, -0.5, 0.5, -0.5, -0.5, -0.5, -0.5, 0.5, -0.5, -0.5, -0.5, -0.5, -0.5, -0.5,
	0.5, -0.5, 0.5, -0.5, -0.5, 0.5, -0.5, 0.5, -0.5, -0.5, -0.5, 0.5, 0.5, -0.5, -0.5, 0.5, -0.5, 0.5, 0.5,
	0.5, 0.5, -0.5, -0.5, 0.5, 0.5, -0.5, 0.5, -0.5, 0.5, 0.5, -0.5, 0.5, 0.5, 0.5, 0.5, -0.5, -0.5, 0.5, -0.5,
	-0.5, 0.5, -0.5, 0.5, 0.5, -0.5, -0.5, -0.5, 0.5, 0.5, -0.5, 0.5, -0.5, -0.5, -0.5, 0.5, -0.5,

	0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0,
	1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, -1.0,
	0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, -1.0, 0.0, 0.0, 	-1.0,
	0.0, 0.0, -1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 	0.0, 1.0,
	0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0,
	0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0, 0.0, 0.0, -1.0,
	
	1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
	1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0,
	1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0,1.0,
	1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0,
	
	0.33333, 0.33333, 0.0, 0.0, 0.0, 0.33333, 0.33333, 0.33333, 0.0, 0.66667,
	0.33333, 0.66667, 0.33333, 0.66667, 0.66667, 1.0, 0.66667, 0.66667, 0.0, 1.0,
	0.33333, 0.66667, 0.0, 0.66667, 1.0, 0.66667, 0.66667, 1.0, 1.0, 1.0,
	0.66667, 0.66667, 0.33333, 0.33333, 0.33333, 0.66667, 0.33333, 0.33333, 0.33333, 0.0,
	0.0, 0.0, 0.33333, 0.33333, 0.0, 0.33333, 0.0, 0.66667, 0.33333, 0.66667,
	0.33333, 1.0, 0.66667, 1.0, 0.0, 1.0, 0.33333, 1.0, 0.33333, 0.66667,
	1.0, 0.66667, 0.66667, 0.66667, 0.66667, 1.0, 0.66667, 0.66667, 0.66667, 0.33333,
	0.33333, 0.33333
};

CubeMesh::CubeMesh() {
	constexpr int uvCount = 2;
	
	this->hasNormal = true;
	this->hasTexcoord = true;
	this->hasTangentSpaceBasis = false; // TODO
	this->init(36, uvCount, 0);

	memcpy(this->vertices, cubeVertexBuffer, this->vertexCount * sizeof(vec3));
	memcpy(this->normals, cubeVertexBuffer + this->vertexCount * 3, this->vertexCount * sizeof(vec3));
	memcpy(this->texcoords, cubeVertexBuffer + this->vertexCount * 3 * 2, this->vertexCount * sizeof(vec2) * uvCount);
}

}
