///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __renderer_h__
#define __renderer_h__

#include <stdio.h>
#include <stack>

#include "ugm/matrix.h"
#include "scene.h"

namespace raygen {

class SceneTransformStack;
class RenderSceneObject;
class RenderMesh;

class Renderer
{
private:
	
protected:
	Renderer();
	~Renderer();

	Scene* scene = NULL;
	Camera defaultCamera;
	
  Matrix4 projectionMatrix;

	SceneTransformStack* transformStack = NULL;
  
  inline Matrix4& getProjectionMatrix() { return this->projectionMatrix; }

  void applyCameraTransform(const Camera& camera);
	void resetTransformMatrices();
  
public:
	std::vector<RenderSceneObject*> renderObjects;
	Matrix4 viewMatrix;

	inline void setScene(Scene* scene) {
    this->scene = scene;
  }
  
  inline const Scene* getScene() const {
    return this->scene;
  }
  
  virtual void render() = 0;
	
//	vec4 toWorldPosition(const vec4 v);
};

class SceneTransformStack {
private:
	std::stack<Matrix4> modelMatrixStack;

	void pushModelMatrix();
	void popModelMatrix();
	
public:
	Matrix4 modelMatrix;
	Matrix4 normalMatrix;
	
	void pushObject(const SceneObject& obj);
	void popObject();
	
	void reset();
};

}

#endif /* renderer_h */
