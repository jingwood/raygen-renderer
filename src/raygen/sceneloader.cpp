///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include <cassert>

#include "sceneloader.h"
#include "ucm/jsonreader.h"
#include "ucm/strutil.h"
#include "ucm/file.h"
#include "ugm/imgcodec.h"
#include "meshloader.h"
#include "fbxloader.h"
#include "polygons.h"

#define FORMAT_TAG_JSON 0x6e6f736a
#define FORMAT_TAG_MIFT 0x7466696d

namespace raygen {

SceneJsonLoader::SceneJsonLoader(SceneResourcePool* resPool) {
	if (resPool != NULL) {
		this->resPool = resPool;
	}
}

SceneJsonLoader::~SceneJsonLoader() {
}

void SceneJsonLoader::setBasePath(const string& basePath) {
	this->basePath = basePath;
	
	if (this->basePath.length() > 0 && !this->basePath.endsWith(PATH_SPLITTER)) {
		this->basePath.append(PATH_SPLITTER);
	}
}

void SceneJsonLoader::transformPath(const string &input, string &output) {
	output.clear();
	
	if (!input.startsWith("sob://")
			&& !input.startsWith("tob://")
			&& !input.startsWith('/')
			&& !input.startsWith("\\")) {
		output.append(this->basePath);
		output.append(PATH_SPLITTER);
	}
	
	output.append(input);
}

color4f SceneJsonLoader::readColorProperty(const JSObject* obj) {
	return color4f((float)obj->getNumberProperty("r", 0.0f),
								 (float)obj->getNumberProperty("g", 0.0f),
								 (float)obj->getNumberProperty("b", 0.0f),
								 (float)obj->getNumberProperty("a", 0.0f));
}

color4f SceneJsonLoader::readColorArray(const std::vector<JSValue>& array) {
	color4f c;
	
	if (array.size() >= 3) {
		if (array[0].type == JSType::JSType_Number) c.r = (float)array[0].number;
		if (array[1].type == JSType::JSType_Number) c.g = (float)array[1].number;
		if (array[2].type == JSType::JSType_Number) c.b = (float)array[2].number;
	}
	
	if (array.size() >= 4) {
		if (array[3].type == JSType::JSType_Number) c.a = (float)array[3].number;
	}
	
	return c;
}

void SceneJsonLoader::readMaterialDefines(const JSObject& jsmats, Archive* bundle) {
	for (const auto& p : jsmats.getProperties()) {
		if (p.second.type == JSType::JSType_Object && p.second.object != NULL) {
			Material* mat = new Material();
			mat->name = p.first;
			this->readMaterial(*mat, *p.second.object, this->resPool, bundle);
//			this->resPool->materials[p.first] = mp;
			this->loadingStack.back().materials[p.first] = mat;
//			this->materialList
//			materials[p.first] = m;
		}
	}
}

void SceneJsonLoader::readMaterial(Material& mat, const JSObject& jsmat, SceneResourcePool* pool, Archive* bundle) {
	string* texPath = NULL;
	if ((texPath = jsmat.getStringProperty("tex")) != NULL && texPath->length() > 0) {
		string filepath;
		this->transformPath(*texPath, filepath);
		
		mat.texturePath = filepath;

		if (pool != NULL) {
			mat.texture = pool->getTexture(filepath, bundle);
		}
		
#ifdef DEBUG
		assert(mat.texture != NULL);
		assert(mat.texture->getImage().width() > 0);
		assert(mat.texture->getImage().width() < 65500);
#endif
	}
	
	string* normalmapPath = NULL;
	if ((normalmapPath = jsmat.getStringProperty("normalmap")) != NULL && normalmapPath->length() > 0) {
		mat.normalmapPath = *normalmapPath;
		
		// TODO: load normalmap from file
	}

	SceneJsonLoader::tryReadVec2Property(jsmat, "texTiling", &mat.texTiling);

	jsmat.tryGetNumberProperty("emission", &mat.emission);
	jsmat.tryGetNumberProperty("glossy", &mat.glossy);
	jsmat.tryGetNumberProperty("roughness", &mat.roughness);
	jsmat.tryGetNumberProperty("transparency", &mat.transparency);
	jsmat.tryGetNumberProperty("refraction", &mat.refraction);
	jsmat.tryGetNumberProperty("refractionRatio", &mat.refractionRatio);
	jsmat.tryGetNumberProperty("spotRange", &mat.spotRange);
	jsmat.tryGetNumberProperty("normalMipmap", &mat.normalMipmap);

	JSValue val;
	
	if ((val = jsmat.getProperty("color")).type != JSType_Unknown) {
		switch (val.type) {
			default:
				break;
				
			case JSType::JSType_String:
				if (val.str != NULL) {
					color4 c;
					tryParseColorString(*val.str, c);
					mat.color = c;
				}
				break;
				
			case JSType::JSType_Array:
				if (val.array != NULL) {
					mat.color = readColorArray(*val.array);
				}
				break;
		}
	}
}

bool SceneJsonLoader::tryParseColorString(const string& str, color4& color) {
	int len = str.length();
	
	if (len < 3) {
		return false;
	}
	
	const char* buf = str.getBuffer();
	
	if ((len == 4 && str[0] == '#')
			|| (len == 5 && str[0] == '#')
			|| (len == 7 && str[0] == '#')
			|| (len == 9 && str[0] == '#')) {
		buf++;
		len--;
	}
	
	switch (len) {
		case 3:
			color = tocolor3f(hex2dec(buf, 3) * 2);
			return true;
		case 4:
			color = tocolor4f(hex2dec(buf, 4) * 2);
			return true;
		case 6:
			color = tocolor3f(hex2dec(buf, 6));
			return true;
		case 8:
			color = tocolor4f(hex2dec(buf, 8));
			return true;
	}
	
	return false;
}

bool SceneJsonLoader::tryReadVec3Property(const JSObject& obj, const char* name, vec3* v) {
	const JSValue& val = obj.getProperty(name);
	
	switch (val.type) {
		default:
			break;
			
		case JSType::JSType_Array:
			if (val.array->size() >= 3) {
				v->x = (float)val.array->at(0).number;
				v->y = (float)val.array->at(1).number;
				v->z = (float)val.array->at(2).number;
				return true;
			}
			break;
			
		case JSType::JSType_Object:
			v->x = (float)obj.getNumberProperty("x", 0);
			v->y = (float)obj.getNumberProperty("y", 0);
			v->z = (float)obj.getNumberProperty("z", 0);
			return true;
	}
	
	return false;
}
			
bool SceneJsonLoader::tryReadVec2Property(const JSObject& obj, const char* name, vec2* v) {
	const JSValue& val = obj.getProperty(name);

	switch (val.type) {
		default:
			break;
			
		case JSType::JSType_Array:
			if (val.array->size() >= 2) {
				v->x = (float)val.array->at(0).number;
				v->y = (float)val.array->at(1).number;
				return true;
			}
			break;
			
		case JSType::JSType_Object:
			v->x = (float)obj.getNumberProperty("x", 0);
			v->y = (float)obj.getNumberProperty("y", 0);
			return true;
	}

	return false;
}

void SceneJsonLoader::readMeshDefines(const JSObject* obj, std::vector<Mesh> meshes) {
	// TODO
}

void SceneJsonLoader::readMesh(SceneObject& obj, const string& meshPath, Archive* bundle) {
//	Mesh* mesh = this->loadMeshFile(obj, meshPath, bundle);
	string filepath;
	this->transformPath(meshPath, filepath);
    
	Mesh* mesh = this->resPool->loadMeshFromFile(filepath, bundle);
	if (mesh != NULL) {
		obj.addMesh(*mesh);
	}
}

//Mesh* SceneJsonLoader::loadMeshFile(SceneObject& obj, const string &path, Archive* bundle) {
//	Mesh* mesh = new Mesh();
//
//	string finalPath;
//	finalPath.append(this->basePath);
//	finalPath.append(path);
//	
//#if _WIN32
//	finalPath.replace('/', '\\');
//#endif // _WIN32
//	
//	if (finalPath.endsWith('.fbx', StringComparingFlags::SCF_CASE_INSENSITIVE)) {
//#if defined(FBX_SUPPORT)
//		SceneFBXLoader loader;
//		loader.loadAsChildren(&obj, finalPath);
//#endif /* FBX_SUPPORT */
//	} else {
//		MeshLoader::load(*mesh, finalPath);
//	}
//	
//	return mesh;
//}

void SceneJsonLoader::readSceneObject(SceneObject& obj, const JSObject& jsobj, Archive* bundle) {
	
	this->loadingStack.push_back(LoadingStack());

	JSObject* matObj = jsobj.getObjectProperty("_materials");
	
	if (matObj != NULL) {
		this->readMaterialDefines(*matObj, bundle);
	}
	
	string* jsBundleURI = jsobj.getStringProperty("_bundle");
	if (jsBundleURI != NULL && !jsBundleURI->isEmpty()) {
		string bundleFilepath;
		this->transformPath(*jsBundleURI, bundleFilepath);
		
		Archive* archive = this->resPool->loadArchive(bundleFilepath, bundleFilepath);
		
		string manifest;
		archive->getTextChunkData(0x1, FORMAT_TAG_MIFT, &manifest);
		
		JSONReader reader(manifest);
		JSObject* bundleJSChildRoot = reader.readObject();
		
		for (auto& p : bundleJSChildRoot->getProperties()) {
			if (p.first == "_materials") {
				if (p.second.type == JSType_Object && p.second.object != NULL) {
					this->readMaterialDefines(*p.second.object, archive);
				}
			} else if(p.first != "_models"
								&& p.second.type == JSType::JSType_Object && p.second.object != NULL) {
				
				SceneObject* child = new SceneObject();
				this->readSceneObject(*child, *p.second.object, archive);
				
				child->setName(p.first);
				obj.addObject(*child);
			}
		}
		
		delete bundleJSChildRoot;
	}

	Camera* camera;
	
	if ((camera = dynamic_cast<Camera*>(&obj)) != NULL) {
		
		if (jsobj.hasProperty("fieldOfView", JSType::JSType_Number)) {
			camera->fieldOfView = (float)jsobj.getNumberProperty("fieldOfView");
		}
		
		if (jsobj.hasProperty("depthOfField", JSType::JSType_Number)) {
			camera->depthOfField = (float)jsobj.getNumberProperty("depthOfField");
		}
		
		if (jsobj.hasProperty("aperture", JSType::JSType_Number)) {
			camera->aperture = (float)jsobj.getNumberProperty("aperture");
		}
		
		if (jsobj.hasProperty("focusOn", JSType::JSType_String)) {
			const string* focusAtObjName = jsobj.getStringProperty("focusOn");
			if (focusAtObjName != NULL && !focusAtObjName->isEmpty()) {
				camera->focusOnObjectName = *focusAtObjName;
			}
		}
	}
	
	for (const auto& p : jsobj.getProperties()) {
		const string& key = p.first;
		const JSValue& val = p.second;

		if (key == "_materials" || key == "_bundles") {
			// ignore these properties since handled before loop
		} else if (key == "location") {
			SceneJsonLoader::tryReadVec3Property(jsobj, key, &obj.location);
		}
		else if (key == "angle") {
			SceneJsonLoader::tryReadVec3Property(jsobj, key, &obj.angle);
		}
		else if (key == "scale") {
			SceneJsonLoader::tryReadVec3Property(jsobj, key, &obj.scale);
		}
		else if (key == "mesh") {
			// read mesh from file path
			if (val.type == JSType_String && val.str != NULL) {
				this->readMesh(obj, *val.str, bundle);
			}
			// read multiple mesh from path
			else if (val.type == JSType::JSType_Array) {
				for (const JSValue& meshItem : *val.array) {
					if (meshItem.type == JSType::JSType_String
							&& meshItem.str != NULL) {
						this->readMesh(obj, *meshItem.str, bundle);
					}
				}
			}
			// read mesh define
			else if (val.type == JSType::JSType_Object) {
				string* meshType = val.object->getStringProperty("type");
				if (*meshType == "plane") {
					obj.addMesh(*new PlaneMesh());
				} else if (*meshType == "cube") {
					obj.addMesh(*new CubeMesh());
				}
			}
		}
		else if (key == "mat") {
			if (this->materialReadingHandler != NULL) {
				this->materialReadingHandler(obj, jsobj, this->meshLoadHandlerUserData);
			} else {
				if (val.type == JSType::JSType_String && val.str != NULL) {
					const string& matName = *val.str;
					
					Material* mat = this->findMaterialByName(matName);
					if (mat != NULL) {
						obj.material = *mat;
					}

//					obj->material.name = matName;

				} else if (val.type == JSType::JSType_Object && val.object != NULL) {
					SceneJsonLoader::readMaterial(obj.material, *val.object, this->resPool);
				}
			}
		}
		else if (key == "visible" && val.type == JSType::JSType_Boolean) {
			obj.visible = val.boolean;
		}
		else if (key == "mainCamera") {
			Camera* camera = new Camera();
			this->readSceneObject(*camera, *val.object, bundle);
			camera->setName(key);
			obj.addObject(*camera);
		}
		else if (key == "_generateLightmap") {
			obj._generateLightmap = true;
		}
		else if (key == "mainCamera") {
			Camera* child = new Camera();
			this->readSceneObject(*child, *val.object, bundle);
			
			child->setName(key);
			obj.addObject(*child);
		}
		else if (val.type == JSType::JSType_Object && val.object != NULL) {

			SceneObject* child = NULL;
			
			int type = (int)val.object->getNumberProperty("type");
			switch (type) {
				case 15: /* RefRange */
					child = new ReflectionMapObject();
					break;
				case 801: /* Camera */
					child = new Camera();
					break;
					
				default:
					child = new SceneObject();
					break;
			}
			
			this->readSceneObject(*child, *val.object, bundle);
			
			child->setName(key);
			obj.addObject(*child);
		}
	}
	
	this->loadingStack.pop_back();
}

Camera* findMainCamera(SceneObject& obj) {
	for (auto* child : obj.getObjects()) {
		if (child == NULL) continue;

		Camera* camera = NULL;

		if (child->getName() == "mainCamera" && (camera = dynamic_cast<Camera*>(child)) != NULL) {
			return camera;
		}

		camera = findMainCamera(*child);
		if (camera != NULL) return camera;
	}

	return NULL;
}

void SceneJsonLoader::load(const string& jsonPath, Scene& scene) {
	this->jsonFilePath = jsonPath;

	if (resPool == NULL) {
		resPool = &SceneResourcePool::instance;
	}

	if (this->basePath.isEmpty()) {
		File file(jsonPath);
		this->setBasePath(file.getPath());
	}
	
//	scene.resPool.basePath = this->basePath;
	
	string json;
	File::readTextFile(this->jsonFilePath, json);
	
	if (json.isEmpty()) {
		throw Exception("scene file is empty");
	}

	SceneObject* rootObj = this->loadObject(json);
	
	if (rootObj != NULL) {
		for (auto child : rootObj->getObjects()) {
			child->setParent(NULL);
			scene.addObject(*child);
		}

		Camera* mainCamera = findMainCamera(*rootObj);
		if (mainCamera != NULL) {
			scene.mainCamera = mainCamera;
		}

		rootObj->objects.clear();
		delete rootObj;
		rootObj = NULL;
	}
	
}



SceneObject* SceneJsonLoader::loadObject(const string& json, Archive* bundle) {
//	if (this->resPool == NULL) {
//		this->resPool = new SceneResourcePool();
//	}
	
	JSONReader reader(json);
	JSObject* jsobj = reader.readObject();
	
	SceneObject* obj = this->loadObject(*jsobj, bundle);
	
	delete jsobj;
	jsobj = NULL;
	
	return obj;
}

SceneObject* SceneJsonLoader::loadObject(const JSObject& jsobj, Archive* bundle) {

	SceneObject* obj = new SceneObject();
	this->readSceneObject(*obj, jsobj, bundle);
	return obj;
	
//	this->loadingStack.push_back(LoadingStack());
//	std::vector<Mesh> meshes;
//
//	for (const auto& p : jsobj.getProperties()) {
//		if (p.first == "_materials") {
//			// ignore since read before
//			continue;
//		}
//		else if (p.first == "_meshes") {
//			this->readMeshDefines(p.second.object, meshes);
//		}
//		else if (p.first == "mainCamera") {
//			Camera* camera = new Camera();
//			camera->setName(p.first);
//			this->readSceneObject(camera, *p.second.object, bundle);
//			return camera;
//		}
//		else if (p.second.type == JSType::JSType_Object && p.second.object != NULL) {
//			SceneObject* child = new SceneObject();
//			child->setName(p.first);
//			this->readSceneObject(child, *p.second.object, bundle);
//			return child;
//		}
//	}

	// unknow why this here, we don't need it
	//this->readSceneObject(obj, jsobj, bundle);

//	this->loadingStack.pop_back();

//	return NULL;
}

SceneObject* SceneJsonLoader::createObjectFromBundle(const string& path) {

	if (resPool == NULL) {
		resPool = &SceneResourcePool::instance;
	}
	
	Archive* archive = this->resPool->loadArchive(path);
	
	if (archive == NULL) {
		return NULL;
	}

	string manifest;
	archive->getTextChunkData(1, FORMAT_TAG_MIFT, &manifest);
	
	SceneObject* obj = this->loadObject(manifest, archive);
	
	if (obj->objects.size() == 1) {
		SceneObject* child = obj->objects[0];
		
		obj->removeObject(*child);
		delete obj;
		obj = NULL;
		
		return child;
	}
	
	return obj;
}

//////////////////// SceneJsonWriter ////////////////////

//void SceneJsonWriter::writeObject(const SceneObject& obj, bool insoba) {
//
//	writer.format = JSONOutputFormat::defaultFormat;
//
//	writer.beginObject();
//
////	// materials
////	const std::vector<ObjMaterial>& materials = this->objReader.getMaterials();
////
////	if (materials.size() > 0) {
////		// materials
////		writer.beginObjectWithKey("_materials");
////
////		for (const ObjMaterial& m : materials) {
////			this->writeMaterial(m, insoba);
////		}
////
////		writer.endObject();
////	}
//
//	for (SceneObject* child : obj.objects) {
//		writeObject(child, insoba);
//	}
//
//	writer.endObject();
//}
//
//void SceneJsonWriter::writeMaterial(const Material& m) {
//	this->writer.beginObjectWithKey(m.name);
//
//	// tex
//	const string texFile = m.texturePath;
//	
//	if (!texFile.isEmpty()) {
//		this->writeTexture(writer, "tex", texFile);
//	}
//
//	// normal-map
//	const string normalFile = mat.getNormalmapFilename();
//
//	if (!normalFile.isEmpty()) {
//		writeTexture(writer, "normalmap", normalFile, insoba);
//	}
//
//	// color
//	if (m.color != colors::black) {
//		string colorValue;
//		colorValue.appendFormat("[%f, %f, %f]", m.color.r, m.color.g, m.color.b);
//		writer.writeCustomProperty("color", colorValue.getBuffer());
//	}
//
//	// transparency
//	if (m.transparency > 0) {
//		writer.writeProperty("transparency", m.transparency);
//	}
//
//	writer.endObject();
//}
//
//void SceneJsonWriter::writeTexture(const string& name, const string &texFile) {
//	
//	if (!this->embedResources) {
//		char texPath[300];
//		_strcpy(texPath, texFile.getBuffer());
//		convertToUnixRelativePath(texPath, this->workpath);
//
//		writer.writeProperty(name, texPath);
//	}
//	else {
//
//		string texFullPath;
//		if (texFile.startsWith(PATH_SPLITTER)) {
//			texFullPath = texFile;
//		} else {
//			texFullPath.append(this->fromFile.getPath());
//			if (!texFile.startsWith(PATH_SPLITTER)) texFullPath.append(PATH_SPLITTER);
//			texFullPath.append(texFile);
//		}
//
//		ChunkEntry* entry = archiveInfo.archive.newChunk(0x20786574);
//
//		if (!texFile.endsWith(".bmp", StringComparingFlags::SCF_CASE_INSENSITIVE)) {
//			entry->isCompressed = false;
//		}
//
//		if (this->textureToJPEG) {
//			Texture* tex = resPool.getTexture(texFullPath);
//			Image img3b(PixelDataFormat::PDF_RGB, 8);
//			Image::copy(tex->getImage(), img3b);
//			saveImage(img3b, *entry->stream, ImageCodecFormat::ICF_JPEG);
//		} else {
//			FileStream fs = File::openRead(texFullPath);
//			Stream::copy(fs, *entry->stream);
//		}
//
//		writer.writeProperty(name, "sob://%s/%08x", insoba ? "__this__" : this->outputJsonObjectName.getBuffer(), entry->uid);
//		archiveInfo.archive.updateAndCloseChunk(entry);
//	}
//}

//////////////////// RendererSceneLoader ////////////////////

void RendererSceneLoader::load(Renderer &renderer, Scene* scene, const string &path) {
//	if (scene == NULL) {
//		scene = new Scene();
//	}
	
	if (path.endsWith(".fbx", StringComparingFlags::SCF_CASE_INSENSITIVE)) {
#ifdef FBX_SUPPORT
		SceneFBXLoader fbxLoader;
		fbxLoader.load(*scene, path);
#endif /* FBX_SUPPORT */
	} else {
		
//		SceneResourcePool* pool = new SceneResourcePool(); // FIXME: release
		
		SceneJsonLoader jsonLoader(&SceneResourcePool::instance);
		jsonLoader.load(path, *scene);

		renderer.setScene(scene);
		
		//	Camera* camera = new Camera();
		//	scene->addObject(camera);
		//	scene->setMainCamera(camera);
		//  resetMainCamera(scene);
	}
}

}
