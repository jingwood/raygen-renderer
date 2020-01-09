///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#if defined(FBX_SUPPORT)

#ifndef fbxloader_h
#define fbxloader_h

#include <stdio.h>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpragma-pack"
#include <fbxsdk.h>
#pragma clang diagnostic pop

#include "scene.h"

namespace raygen {

class SceneFBXLoader {
private:
	FbxManager* fbxManager = NULL;
	FbxImporter* fbxImporter = NULL;
	
	string filePath;
	Scene* scene = NULL;
	
	void loadNodeChildren(FbxNode& node, SceneObject* parent = NULL);
	SceneObject* loadNode(FbxNode& node, SceneObject* parent = NULL);

	Material* readMaterial(FbxSurfaceMaterial& fbxMat);
	Texture* readTexture(FbxTexture& fbxTex);

public:
	SceneFBXLoader();
	~SceneFBXLoader();

	SceneResourcePool pool;

	bool load(Scene& scene, const string& path);
	bool loadAsChildren(SceneObject* obj, const string& path);
};

}

#endif /* fbxloader_h */

#endif /* FBX_SUPPORT */
