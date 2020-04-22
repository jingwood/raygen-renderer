///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef obj_reader_hpp
#define obj_reader_hpp

#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <map>

#include "ucm/lexer.h"
#include "ucm/console.h"
#include "ucm/file.h"
#include "ugm/vector.h"
#include "ugm/color.h"
#include "mesh.h"

#if _WIN32
#define _strcpy strcpy_s
#else
#define _strcpy strcpy
#endif // WIN32

#define LINE_BUFFER_LENGTH 300

using namespace ucm;
using namespace ugm;

namespace raygen {

class ObjFileReader;

class ObjMaterial {
  friend ObjFileReader;
  
private:

public:
	string name;
	string textureFilename;
	string normalmapFilename;
	color3f ambient;
	color3f diffuse;
	color3f specular;
	float shininess;
	float transparency;
	float normalmapIntensity;

	ObjMaterial() { }
	
	inline void setName(const string& name) { this->name = name; }
  inline void setAmbient(const color3& ambient) { this->ambient = ambient; }
  inline void setDiffuse(const color3& diffuse) { this->diffuse = diffuse; }
  inline void setSpecular(const color3& specular) { this->specular = specular; }
  inline void setShininess(const float shininess) { this->shininess = shininess; }
	inline void setTextureFilename(const string& filename) { this->textureFilename = filename; }
	inline void setNormalmapFilename(const string& filename) { this->normalmapFilename = filename; }
	
  inline const string& getName() const { return this->name; }
  inline const string& getTextureFilename() const { return this->textureFilename; }
	inline const string& getNormalmapFilename() const { return this->normalmapFilename; }

	void reset();
};

class ObjObject;

class ObjObject {
  friend ObjFileReader;
  
private:
  string name;
  
//  std::vector<unsigned int> vertexIndices;
//  std::vector<unsigned int> normalIndices;
//  std::vector<unsigned int> texcoordIndices;

	Mesh mesh;
	std::vector<vec3> vertices;
	std::vector<vec3> normals;
	std::vector<vec2> texcoords;
	
	bool hasNormal = false;
	bool hasTexcoord = false;

	vec3 location;
  vec3 size;
  vec3 origin;
	
	std::vector<ObjObject*> children;
	const ObjObject* parent = NULL;
	
  const ObjMaterial* material = NULL;

	std::vector<string> groupNames;
	
  ObjObject() {
  }

public:
	string selectedMatName;
	bool hasReadingError = false;

	ObjObject(const ObjObject& obj) {
		*this = obj;
	}
  ~ObjObject() {
  }
  
  inline const string& getName() const { return this->name; }
  inline void setName(const char* name) { this->name = name; }
	
	const std::vector<ObjObject*>& getChildren() { return this->children; }
	ObjObject* findChildrenByName(const string& name);
	
	Mesh& getMesh() { return this->mesh; }
	
	inline void setLocation(const vec3& loc) { this->location = loc; }
	inline const vec3& getLocation() const { return this->location; }
	
  inline const vec3& getSize() const { return this->size; }
  inline const vec3& getOrigin() const { return this->origin; }
  
  inline const ObjMaterial* getMaterial() const { return this->material; }
  inline void setMaterial(const ObjMaterial* material) { this->material = material; }
};

class ObjFileReader
{
private:
  File* file = NULL;
  FileStream* stream = NULL;
  
  char line[LINE_BUFFER_LENGTH];
	uint lineLength = 0, lineNumber = 0;
	Lexer surfaceLineLexer;
	Lexer groupNameLexer;

  typedef vec2 texcoord;
  
  std::vector<vec3> readVertexs;
  std::vector<vec3> readNormals;
  std::vector<texcoord> readTexcoords;
  std::vector<uint> readIndexes;
	
	std::vector<uint> readVertexIndexes;
	std::vector<uint> readNormalIndexes;
	std::vector<uint> readTexcoordIndexes;
	
	BoundingBox bbox;
  bool firstVertex = true;
	bool globleAutoScale = false;
  
  ObjObject* currentObject = NULL;
  std::vector<ObjObject*> rootObjects;
//	std::vector<ObjObject*> objects;
	std::vector<ObjObject*> errorObjects;
  std::vector<ObjMaterial> materials;

	bool firstObjectSurfaceData = true;
	bool stopOnError = false;
	bool hasError = false;

  inline bool nextLine() {
    bool hasMore = stream->readLine(this->line, LINE_BUFFER_LENGTH);
		if (hasMore) {
			this->lineLength = (uint)strlen(this->line);
			this->lineNumber++;
		}
		return hasMore;
  }
  
  inline bool isLine(const char* tag) {
    const uint len = (uint)(size_t)strlen(tag);
    
    uint i = 0;
    
    for (; i < len; i++) {
      if (this->line[i] != tag[i]) {
        return false;
      }
    }
    
    return i < this->lineLength && line[i] == ' ';
  }
  
  bool readSurfaceLine();
  void finalizeObject();
	void createObjectMesh(ObjObject& obj);
 
  void readMaterialLibrary(const string& matlibPath);
	
	ObjObject* findObjectByName(const string& name);
	bool sameNameObjectAlreadyExist(const string& name);

	const ObjMaterial* getMaterialByName(const string& name) const;
  
public:
  //ObjFileReader();
  ~ObjFileReader();
	
	Console* console = NULL;

	bool makeIndex = false;
  bool alignToOrigin = true;
	
	void read(const char* filename);

	inline void setStopOnError(const bool value) { this->stopOnError = value; }
	inline bool error() { return this->hasError; }
	void markObjectError(ObjObject& obj);
	
	inline const std::vector<ObjObject*>& getObjects() const { return this->rootObjects; }
	inline const std::vector<ObjObject*>& getErrorObjects() const { return this->errorObjects; }
	
	inline const std::vector<ObjMaterial>& getMaterials() const {
		return this->materials;
	}
  
  inline const BoundingBox& getBoundingBox() const { return this->bbox; }
};

}

#endif /* obj_reader_hpp */
