///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef bakerenderer_h
#define bakerenderer_h

#include <stdio.h>
#include "rayrenderer.h"
#include "cubetex.h"

namespace raygen {

class BakeRenderer : public RayRenderer {
	
private:

	void bakeMeshThread(const Mesh& mesh, const int threadId);
	void bakeMeshThread2(const Mesh& mesh, const int threadId);
	void bakeMeshThread3(const Mesh& mesh, const int threadId);
	color3 bakeMeshFragment(const RayRenderTriangle& rt, const vec2& uv);
	color3 bakePoint(const RayRenderTriangle& rt, const vec2& uv);
	
	KDNode2D<Triangle2D> tree;
	void fillVertex(const RayRenderTriangle& rt, const vec2& v);
	byte* imgbits = NULL;

public:
	float margin = 2;
	
	~BakeRenderer();
	
	void prepareBake();
	void clearRenderResult();
	void bakeMesh(const Mesh& mesh);
	void bakeMesh3(const Mesh& mesh);
	
//	BoundingBox findMaximalSpaceFromPoint(const vec3& point);
//	float findMaximalSpaceFromSurface(const vec3& normal);
	
	void bakeCubeTexture(CubeTexture& cubetex, const vec3& cameraLocation);

	void bakeTest();
};

}

#endif /* bakerenderer_h */
