///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "renderer.h"

namespace raygen {

Renderer::Renderer() {
	this->transformStack = new SceneTransformStack();
}

Renderer::~Renderer() {
	if (this->transformStack != NULL) {
		delete this->transformStack;
		this->transformStack = NULL;
	}
}

void Renderer::applyCameraTransform(const Camera& obj) {
  this->viewMatrix.rotate(-obj.angle.x, -obj.angle.y, -obj.angle.z);
	this->viewMatrix.translate(-obj.location.x, -obj.location.y, -obj.location.z);
	this->viewMatrix.scale(1.0f / obj.scale.x, 1.0f / obj.scale.y, 1.0f / obj.scale.z);
}

void Renderer::resetTransformMatrices() {
	this->viewMatrix.loadIdentity();
	this->transformStack->reset();
}

//vec4 Renderer::toWorldPosition(const vec4 v) {
//	return v * this->viewMatrix;
//}

///////////////// SceneTransformStack /////////////////

void SceneTransformStack::pushModelMatrix() {
	this->modelMatrixStack.push(this->modelMatrix);
}

void SceneTransformStack::popModelMatrix() {
	this->modelMatrix = this->modelMatrixStack.top();
	this->modelMatrixStack.pop();
}

void SceneTransformStack::pushObject(const SceneObject& obj) {
	this->pushModelMatrix();
	
	this->modelMatrix.translate(obj.location.x, obj.location.y, obj.location.z);
	this->modelMatrix.rotate(obj.angle.x, obj.angle.y, obj.angle.z);
	this->modelMatrix.scale(obj.scale.x, obj.scale.y, obj.scale.z);
	
	this->normalMatrix = this->modelMatrix;
	this->normalMatrix.inverse();
	this->normalMatrix.transpose();
}

void SceneTransformStack::popObject() {
	this->popModelMatrix();
}

void SceneTransformStack::reset() {
	while (this->modelMatrixStack.size() > 0) {
		this->modelMatrixStack.pop();
	}
	
	this->modelMatrix.loadIdentity();
	this->normalMatrix = this->modelMatrix;
}

}