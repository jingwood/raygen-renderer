///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __scene_loader_h__
#define __scene_loader_h__

#include <vector>

#include "ucm/string.h"
#include "ucm/jstypes.h"
#include "ucm/jsonwriter.h"
#include "scene.h"
#include "renderer.h"

namespace raygen {

typedef void MaterialLoadHandler(SceneObject& obj, const JSObject& matobj, void* userData);

class SceneJsonLoader
{
private:
	string jsonFilePath;
	
	string basePath;
	void transformPath(const string& input, string& output);
	
//	std::map<string, Mesh*> meshes;
//	std::map<string, Material> materialList;
//	std::map<string, Texture*> texturePool;
//	std::map<string, Archive*> archives;
	
	struct LoadingStack {
		std::map<string, Material*> materials;
	};
	std::vector<LoadingStack> loadingStack;
	
	Material* findMaterialByName(const string& name) {
		for (long int i = this->loadingStack.size() - 1; i >= 0; i--) {
			const auto& list = loadingStack[i].materials;
			const auto& it = list.find(name);
			
			if (it != list.end()) {
				return it->second;
			}
		}
		return NULL;
	}
	
	static color4f readColorProperty(const JSObject* obj);
	static color4f readColorArray(const std::vector<JSValue>& array);
	static bool tryParseColorString(const string& str, color4& color);
	
	void readMeshDefines(const JSObject* obj, std::vector<Mesh> meshes);
	
	void readMesh(SceneObject& obj, const string& meshPath, Archive* bundle = NULL);

	void readSceneObject(SceneObject& obj, const JSObject& json, Archive* bundle = NULL);

//	Mesh* loadMeshFile(SceneObject& obj, const string& path, Archive* bundle = NULL);

public:
	SceneResourcePool* resPool = &SceneResourcePool::instance;

	void* meshLoadHandlerUserData = NULL;
	MaterialLoadHandler* materialReadingHandler = NULL;
	
	SceneJsonLoader(SceneResourcePool* resPool = NULL);
	~SceneJsonLoader();
	
	void setBasePath(const string& basePath);
	
	void load(const string& jsonPath, Scene& scene);
	SceneObject* loadObject(const string& json, Archive* bundle = NULL);
	SceneObject* loadObject(const JSObject& jsobj, Archive* bundle = NULL);

	static bool tryReadVec3Property(const JSObject& obj, const char* name, vec3* v);
	static bool tryReadVec2Property(const JSObject& obj, const char* name, vec2* v);

	inline void pushLoadingStack() { this->loadingStack.push_back(LoadingStack()); }
	void readMaterialDefines(const JSObject& jsmats, Archive* bundle = NULL);
	void readMaterial(Material& mat, const JSObject& obj, SceneResourcePool* pool, Archive* bundle = NULL);
	
	SceneObject* createObjectFromBundle(const string& path);
};

//class SceneJsonWriter {
//private:
//	JSONWriter& writer;
//	
//public:
//	bool insoba;
//	
//	void geneareteJson(const SceneObject& obj);
//	void writeMaterial(const Material& m);
//	void writeTexture(const string& name, const string &texFile);
//};

class RendererSceneLoader {
private:
public:
	void load(Renderer& renderer, Scene* scene, const string& path);
};

}

#endif /* __scene_loader_h__ */
