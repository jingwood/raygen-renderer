///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifdef FBX_SUPPORT

#include "fbxloader.h"

namespace raygen {

SceneFBXLoader::SceneFBXLoader() {
	this->fbxManager = FbxManager::Create();
	
	FbxIOSettings *ios = FbxIOSettings::Create(fbxManager, IOSROOT);
	fbxManager->SetIOSettings(ios);
	
	this->fbxImporter = FbxImporter::Create(fbxManager, "");
}

SceneFBXLoader::~SceneFBXLoader() {
	if (this->fbxManager != NULL) {
		this->fbxManager->Destroy();
		this->fbxManager = NULL;
	}
}

bool SceneFBXLoader::load(Scene& scene, const string& path) {
	this->scene = &scene;
	
	return this->loadAsChildren(NULL, path);
}

bool SceneFBXLoader::loadAsChildren(SceneObject* obj, const string& path) {
	
	this->filePath = path;
	
	if (!this->fbxImporter->Initialize(path, -1, this->fbxManager->GetIOSettings())) {
		printf("Call to FbxImporter::Initialize() failed.\n");
		printf("Error returned: %s\n\n", this->fbxImporter->GetStatus().GetErrorString());
		return false;
	}
	
	FbxScene* fbxScene = FbxScene::Create(this->fbxManager, "scene");
	
	this->fbxImporter->Import(fbxScene);
	this->fbxImporter->Destroy();
	
	if (fbxScene->GetGlobalSettings().GetSystemUnit() == FbxSystemUnit::cm) {
		FbxSystemUnit::ConversionOptions co;
		co.mConvertRrsNodes = false;
		co.mConvertLimits = true;
		co.mConvertClusters = true;
		co.mConvertLightIntensity = true;
		co.mConvertPhotometricLProperties = true;
		co.mConvertCameraClipPlanes = true;
		
		FbxSystemUnit::m.ConvertScene(fbxScene, co);
	}
	
	FbxGeometryConverter converter(this->fbxManager);
	converter.Triangulate(fbxScene, true);

	FbxNode* rootNode = fbxScene->GetRootNode();
	if (rootNode == NULL) {
		return false;
	}
	
	loadNode(*rootNode, obj);

	return true;
}

void SceneFBXLoader::loadNodeChildren(FbxNode& node, SceneObject* parent) {
	for (int i = 0; i < node.GetChildCount(); i++) {
		FbxNode* childNode = node.GetChild(i);
		if (childNode != NULL) {
			this->loadNode(*childNode, parent);
		}
	}
}

SceneObject* SceneFBXLoader::loadNode(FbxNode& node, SceneObject* parent) {
	string name = node.GetName();

#if DEBUG
	printf("%s:\n", name.c_str());
#endif /* DEBUG */

	SceneObject* sceneObject = NULL;

	const FbxCamera* fbxCamera = node.GetCamera();
	if (fbxCamera != NULL) {
		Camera* camera = new Camera();
		
		camera->location = vec3(0, 2.2, 6.0);
		camera->angle = vec3(-14, 0, 0);
		camera->fieldOfView = 170;
		
		if (this->scene->mainCamera == NULL) {
			this->scene->mainCamera = camera;
		}
	
		sceneObject = camera;
	}
	
//	FbxProperty prop = node.GetFirstProperty();
//	while(prop.IsValid()) {
//		printf("  --- prop ---> %s\n", prop.GetName().Buffer());
//		prop = node.GetNextProperty(prop);
//	}
	
	FbxProperty p = node.FindProperty("t_refmap_range");
	if (p.IsValid()) {
		sceneObject = new ReflectionMapObject();
	}
	
	const FbxMesh* fbxMesh = node.GetMesh();
	if (fbxMesh != NULL) {
		if (sceneObject == NULL) {
			sceneObject = new SceneObject();
		}
		
		Mesh* mesh = new Mesh();
		
		FbxArray<fbxsdk::FbxVector4> normals;
		bool getNormalsSuccessed = fbxMesh->GetPolygonVertexNormals(normals);
		
		if (getNormalsSuccessed && normals.GetCount() > 0) {
			mesh->hasNormal = true;
		}
		
		fbxsdk::FbxStringList uvSetNameList;
		fbxMesh->GetUVSetNames(uvSetNameList);
		
		FbxArray<fbxsdk::FbxVector2> uvs;

		if (uvSetNameList.GetCount() > 0) {
			const char* uvName = uvSetNameList.GetStringAt(0);
			fbxMesh->GetPolygonVertexUVs(uvName, uvs);
			
			if (uvs.GetCount() > 0) {
				mesh->hasTexcoord = true;
			}
		}
		
		mesh->init(fbxMesh->GetPolygonVertexCount(), uvSetNameList.GetCount());

		const FbxVector4* controlPoints = fbxMesh->GetControlPoints();
		
		const int* indices = fbxMesh->GetPolygonVertices();
		
		for (int i = 0; i < fbxMesh->GetPolygonVertexCount(); i++) {
			const FbxVector4& cp = controlPoints[indices[i]];
			vec3 v = vec3(cp[0], cp[1], cp[2]);
			mesh->vertices[i] = v;

			if (mesh->hasNormal) {
				mesh->normals[i] = vec3(normals[i][0], normals[i][1], normals[i][2]);
			}
			
			if (mesh->hasTexcoord) {
				mesh->texcoords[i] = vec2(uvs[i][0], -uvs[i][1]);
			}
		}

		sceneObject->addMesh(*mesh);
	}
	
	if (sceneObject == NULL) {
		sceneObject = new SceneObject();
	}
	
//	for (int i = 0; i < node.GetNodeAttributeCount(); i++) {
//		const FbxNodeAttribute* attr = node.GetNodeAttributeByIndex(i);
//		switch (attr->GetAttributeType()) {
//			default:
//				break;
//
//			case FbxNodeAttribute::EType::eMesh:
//				sceneObject = new SceneObject();
//				break;
//
//			case FbxNodeAttribute::EType::eCamera:
//				sceneObject = new Camera();
//				break;
//		}
//
//		if (sceneObject != NULL) {
//			break;
//		}
//	}
	
	SceneObject& obj = *sceneObject;
	
	if (parent == NULL) {
		this->scene->addObject(obj);
	} else {
		parent->addObject(obj);
	}
	
	obj.setName(name);
	
	if (dynamic_cast<Camera*>(sceneObject) == nullptr) {
		FbxDouble3 translation = node.LclTranslation.Get();
		FbxDouble3 rotation = node.LclRotation.Get();
		FbxDouble3 scaling = node.LclScaling.Get();

		obj.location = vec3(translation[0], translation[1], translation[2]);
		obj.angle = vec3(rotation[0], rotation[1], rotation[2]);
		obj.scale = vec3(scaling[0], scaling[1], scaling[2]);
	}
	
	if (node.GetMaterialCount() > 0) {
		FbxSurfaceMaterial* fbxMat = node.GetMaterial(0);
		
		Material* mat = this->readMaterial(*fbxMat);
		if (mat != NULL) {
			string name = fbxMat->GetName();
			if (name.isEmpty()) {
				name = "mat";
				pool.getAvailableMaterialName(name);
			}
			mat->name = name;
			pool.materials[name] = mat;
			obj.material = *mat;
			delete mat;
		}
	}
	
	const FbxLight* fbxLight = node.GetLight();
	
	if (fbxLight != NULL) {
		obj.material.emission = fbxLight->Intensity;
	
		auto fbxLightColor = fbxLight->Color.Get();
		obj.material.color = color3(fbxLightColor[0], fbxLightColor[1], fbxLightColor[2]);
		
		if (obj.material.emission > 5.0) obj.material.emission = 5.0;
		
		obj.material.spotRange = 60;
#if DEBUG
		printf("    obj.material.color = [%f, %f, %f]\n", obj.material.color.r, obj.material.color.g, obj.material.color.b);
		printf("    emission = %f\n", obj.material.emission);
#endif /* DEBUG */
	}

	obj.visible = node.GetVisibility();
	
//	FbxProperty p = node.FindProperty("t_emission", false);
//	if (p.IsValid()) {
//		FbxString nodeName = p.GetName();
//		printf("%s\n", nodeName.Buffer());
//	}
	
	p = node.FindProperty("t_lightmap");
	if (p.IsValid()) {
		sceneObject->_generateLightmap = true;
	}
	
	this->loadNodeChildren(node, sceneObject);
	
	return sceneObject;
}

Material* SceneFBXLoader::readMaterial(FbxSurfaceMaterial& fbxMat) {
	Material* mat = NULL;
	
	const FbxSurfacePhong* phong = dynamic_cast<const FbxSurfacePhong*>(&fbxMat);
	if (phong != NULL) {
		
		if (mat == NULL) mat = new Material();
		
		mat->color = color3(phong->Diffuse.Get()[0], phong->Diffuse.Get()[1], phong->Diffuse.Get()[2]);
		printf("    -> shininess = %lf\n", phong->SpecularFactor.Get());
	}

	// This only gets the material of type sDiffuse, you probably need to traverse all Standard Material Property by its name to get all possible textures.
	FbxProperty prop = fbxMat.FindProperty(FbxSurfaceMaterial::sDiffuse);
	
	// Check if it's layeredtextures
	int layeredTextureCount = prop.GetSrcObjectCount<FbxLayeredTexture>();
	
	if (layeredTextureCount > 0) {
		
		//	for (int j = 0; j < layeredTextureCount; j++) {
		FbxLayeredTexture* layeredTexture = FbxCast<FbxLayeredTexture>(prop.GetSrcObject<FbxLayeredTexture>(0));
		const int lcount = layeredTexture->GetSrcObjectCount<FbxTexture>();
		
		//	for (int k = 0; k < lcount; k++) {
		if (lcount > 0) {
			FbxTexture* fbxTex = layeredTexture->GetSrcObject<FbxTexture>(0);
			if (fbxTex != NULL) {
				Texture* tex = this->readTexture(*fbxTex);
				if (tex != NULL) {
					if (mat == NULL) mat = new Material();
					mat->texture = tex;
				}
			}
		}
	} else {
		// Directly get textures
		const int textureCount = prop.GetSrcObjectCount<FbxTexture>();
		//		for (int j = 0; j < textureCount; j++) {
		if (textureCount > 0) {
			FbxTexture* fbxTex = prop.GetSrcObject<FbxTexture>(0);
			if (fbxTex != NULL) {
				Texture* tex = this->readTexture(*fbxTex);
				if (tex != NULL) {
					if (mat == NULL) mat = new Material();
					mat->texture = tex;
				}
			}
		}
	}
	
	FbxProperty propTrans = fbxMat.FindProperty(FbxSurfaceMaterial::sTransparencyFactor);
	if (propTrans.IsValid()) {
		const double transparencyFactor = propTrans.Get<FbxDouble>();
		if (transparencyFactor > 0) {
			mat->transparency = transparencyFactor;
#if DEBUG
			printf("    mat.transparency = %lf\n", transparencyFactor);
#endif /* DEBUG */
		}
	}
	
	FbxProperty propShininess = fbxMat.FindProperty(FbxSurfaceMaterial::sShininess);
	if (propShininess.IsValid()) {
		const double fbxShininess = propShininess.Get<FbxDouble>();
		mat->glossy = (float)log10f(fbxShininess) / 2.0f;
#if DEBUG
		printf("    sShininess = %lf\n", fbxShininess);
#endif /* DEBUG */
	}
	
	return mat;
}

Texture* SceneFBXLoader::readTexture(FbxTexture& fbxTex) {
	FbxFileTexture* fbxFileTex = FbxCast<FbxFileTexture>(&fbxTex);
	Texture* tex = NULL;
	
	if (fbxFileTex != NULL) {
		
		string texturePath = fbxFileTex->GetFileName();
		tex = this->pool.getTexture(texturePath);
	}
	
	return tex;
}

}

#endif /* FBX_SUPPORT */
