///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef Scene_h
#define Scene_h

#include <stdio.h>
#include <vector>
#include <map>

#include "ugm/vector.h"
#include "mesh.h"
#include "material.h"
#include "ucm/string.h"
#include "ucm/archive.h"

namespace raygen {

class SceneObject;

class SceneObject
{
protected:
	string name;
	SceneObject* parent = NULL;
	
	bool sameNameObjectAlreadyExist(const string& name) const;
	
public:
	vec3 location = vec3(0.0f, 0.0f, 0.0f);
  vec3 angle = vec3(0.0f, 0.0f, 0.0f);
  vec3 scale = vec3(1.0f, 1.0f, 1.0f);
	
	Material material;
	bool visible = true;
	bool renderable = true;
	
	struct {
		BoundingBox worldBbox; // FIXME: remove this property since world position is not static
		bool _isRootObject = false; // root container object for either toba or fbx
		bool _generateLightmap = false;
	};
	
	std::vector<Mesh*> meshes;
	std::vector<SceneObject*> objects;

	SceneObject() {}
	SceneObject(const string& name): name(name) { }
	virtual ~SceneObject();
  
	inline const string& getName() const { return this->name; }
	inline void setName(const string& name) { this->name = name; }
	
	inline const std::vector<Mesh*>& getMeshes() const {
    return this->meshes;
  }
	
	void removeMesh(const Mesh* mesh);
	void removeAllMeshes();
  
	void addMesh(Mesh& mesh);
	void addObject(SceneObject& obj);
	void removeObject(SceneObject& object);
	
	inline SceneObject* getParent() {
		return this->parent;
	}
	
	inline const SceneObject* getParent() const {
		return this->parent;
	}
	
	inline void setParent(SceneObject* parent) {
		this->parent = parent;
	}
	
	const inline std::vector<SceneObject*>& getObjects() const {
		return this->objects;
	}
	
	inline std::vector<SceneObject*>& getObjects() {
		return this->objects;
	}

	SceneObject* findObjectByName(const string& name);
	void makeUniqueChildName(string& name) const;
	
	bool eachChild(std::function<bool(SceneObject*)> iterator);
	
	void getParentTransform(Matrix4* m) const;
	void getLocalTransform(Matrix4* m) const;
	void getWorldTransform(Matrix4* m) const;
	void getRotationMatrix(Matrix4* mat, bool selfRotation = true) const;
	void applyTransform(const Matrix4& parentTransform);

	vec3 getWorldLocation() const;

	void lookAt(const vec3& dir, const vec3& up);
	vec3 getLookAt() const;
	
	BoundingBox getBoundingBox() const;
	
	SceneObject* clone() const;
};

class Camera : public SceneObject {
public:
  float fieldOfView = 75.0f;
	float viewNear = 0.1f;
	float viewFar = 50.0f;
	float depthOfField = 0.0f;
	float aperture = 1.8f;
	string focusOnObjectName;

	Camera() : SceneObject() {
    this->visible = false;
  }
};

class SceneResourcePool {
private:
	SceneResourcePool();
	~SceneResourcePool();
	
public:
	std::map<string, Mesh*> meshes;
	std::map<string, const Material*> materials;
	std::map<string, Texture*> textures;
	std::map<string, Texture*> normalmaps;
	std::map<string, Archive*> archives;
	
	void getAvailableMaterialName(string& name);



public:
	
	Mesh* getMesh(const string& meshURL);
	Mesh* loadMeshFromFile(const string& meshURL, Archive* archive);
	
	Texture* getTexture(const string& path, Archive* bundle = NULL);
	
	Archive* loadArchive(const string& path);
	Archive* loadArchive(const string& name, const string& path);
	
	void collect(const SceneObject& obj);
	void clear();
	
	static SceneResourcePool instance;
	static SceneResourcePool* getInstance() {
		return &instance;
	}
};

class Scene
{
private:
  std::vector<SceneObject*> objects;
	
public:
	~Scene();

//	SceneResourcePool resPool;
	Camera* mainCamera = NULL;

  void addObject(SceneObject& object);
	void removeObject(SceneObject& object);
	void clearObjects();

  const std::vector<SceneObject*>& getObjects() const;
	std::vector<SceneObject*>& getObjects();
	
	void eachChild(std::function<bool(SceneObject*)> iterator);
	SceneObject* findObjectByName(const string& name);
	
	void applyTransform();
};

class ReflectionMapObject : public SceneObject {
public:
	ReflectionMapObject();
};

}

#endif /* Scene_h */
