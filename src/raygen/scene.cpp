///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "scene.h"

#include <stdio.h>
#include <string>
#include <algorithm>

#include "ucm/file.h"
#include "ugm/imgcodec.h"
#include "meshloader.h"
#include "objreader.h"

#ifdef _WIN32
// Plain sscanf, not sscanf_s — sscanf_s requires a buffer-size argument after
// each %s/%[...] and without it invokes the CRT invalid-parameter handler,
// which __fastfails (0xC0000409) at runtime. Mac/Linux already use sscanf.
#define _CRT_SECURE_NO_WARNINGS
#define _sscanf sscanf
#ifndef PATH_MAX
#define PATH_MAX 350
#endif
#else
#define _sscanf sscanf
#endif /* _WIN32 */

#ifdef FBX_SUPPORT
#include "../utility/fbxloader.h"
#endif /* FBX_SUPPORT */

namespace raygen {

SceneObject::~SceneObject() {
    for (SceneObject* obj : this->objects) {
        delete obj;
    }
    
    //	this->objects.clear();
    
    // don't dealloc mesh since it may be shared with other objects
    //	for (Mesh* mesh : this->meshes) {
    //    delete mesh;
    //  }
    
    //  this->meshes.clear();
}

void SceneObject::addMesh(Mesh& mesh) {
    this->meshes.push_back(&mesh);
}

void SceneObject::removeMesh(const Mesh* mesh) {
    const auto& it = std::find(this->meshes.begin(), this->meshes.end(), mesh);
    if (it != this->meshes.end()) {
        this->meshes.erase(it);
    }
}

void SceneObject::removeAllMeshes() {
    this->meshes.clear();
}

void SceneObject::addObject(SceneObject& obj) {
    obj.parent = this;
    this->objects.push_back(&obj);
}

void SceneObject::removeObject(SceneObject& object) {
    const auto& it = std::find(this->objects.begin(), this->objects.end(), &object);
    if (it != this->objects.end()) {
        this->objects.erase(it);
    }
    object.setParent(NULL);
}

SceneObject* SceneObject::findObjectByName(const string& name) {
    for (SceneObject* obj : this->objects) {
        if (obj->name == name) {
            return obj;
        }
        
        SceneObject* child = obj->findObjectByName(name);
        if (child != NULL) {
            return child;
        }
    }
    
    return NULL;
}

void SceneObject::makeUniqueChildName(string& name) const {
    if (sameNameObjectAlreadyExist(name)) {
        for (int index = 2;; index++) {
            string objName = name;
            objName.appendFormat("_%d", index);
            
            if (!sameNameObjectAlreadyExist(objName)) {
                name = objName;
                return;
            }
        }
    }
}

bool SceneObject::sameNameObjectAlreadyExist(const string& name) const {
    for (const auto* obj : this->objects) {
        if (obj->name.equals(name)) {
            return true;
        }
    }
    
    return false;
}

bool SceneObject::eachChild(std::function<bool(SceneObject*)> iterator) {
    for (auto* child : this->objects) {
        if (!iterator(child)) return false;
    }
    
    for (auto* child : this->objects) {
        if (!child->eachChild(iterator)) {
            return false;
        }
    }
    
    return true;
}

void SceneObject::lookAt(const vec3 &dir, const vec3& up) {
    Matrix4 m;
    m.lookAt(this->getWorldLocation(), dir, up);
    this->angle = -m.extractEulerAngles();
}

void SceneObject::getParentTransform(Matrix4 *m) const {
    if (this->parent != NULL) {
        this->parent->getWorldTransform(m);
    } else {
        *m = Matrix4::identity;
    }
}

void SceneObject::getLocalTransform(Matrix4* m) const {
    m->loadIdentity();
    
    m->translate(this->location.x, this->location.y, this->location.z)
        .rotate(this->angle.x, this->angle.y, this->angle.z)
        .scale(this->scale.x, this->scale.y, this->scale.z);
}

void SceneObject::getWorldTransform(Matrix4* m) const {
    Matrix4 mp, ml;
    this->getParentTransform(&mp);
    this->getLocalTransform(&ml);
    *m = mp * ml;
}

void SceneObject::getRotationMatrix(Matrix4* m, bool selfTransform) const {
    std::vector<const SceneObject*> parentList;
    
    if (selfTransform) {
        parentList.push_back(this);
    }
    
    SceneObject* parent = this->parent;
    
    while (parent != NULL) {
        parentList.push_back(parent);
        parent = parent->parent;
    }
    
    for (int i = (int)parentList.size() - 1; i >= 0; i--) {
        const SceneObject* obj = parentList[i];
        
        m->rotate(obj->angle.x, obj->angle.y, obj->angle.z);
    }
}

void SceneObject::applyTransform(const Matrix4& parentTransform) {
    Matrix4 selfTransform;
    this->getLocalTransform(&selfTransform);
    selfTransform = parentTransform * selfTransform;
    
    if (this->meshes.size() > 0) {
        for (Mesh* mesh : this->meshes) {
            mesh->applyTransform(selfTransform);
        }
        
        this->location = vec3::zero;
        this->angle = vec3::zero;
        this->scale = vec3::one;
    }
    
    for (SceneObject* obj : this->objects) {
        obj->applyTransform(selfTransform);
    }
}

vec3 SceneObject::getWorldLocation() const {
    Matrix4 mp;
    this->getParentTransform(&mp);
    
    return (vec4(this->location, 1.0f) * mp).xyz;
}

vec3 SceneObject::getLookAt() const {
    Matrix4 mat;
    this->getRotationMatrix(&mat, false);
    
    vec3 dir, up;
    mat.extractLookAtVectors(&dir, &up);
    return dir;
}

BoundingBox SceneObject::getBoundingBox() const {
    //	BoundingBox bbox;
    //	bbox.initTo(this->location);
    //
    //	for (const Mesh* mesh : this->meshes) {
    //		for (int i = 0; i < mesh->getTriangleCount(); i++) {
    //			vec3 v1, v2, v3;
    //			mesh->getVertex(i, &v1, &v2, &v3);
    //			BoundingBox bmesh = BoundingBox::fromTriangle(v1, v2, v3);
    //			bbox.expandTo(bmesh);
    //		}
    //	}
    //
    //	bbox.finalize();
    //
    BoundingBox bbox;
    
    if (this->meshes.size() > 0) {
        bbox = this->meshes[0]->bbox;
        
        for (int i = 1; i < this->meshes.size(); i++){
            bbox.expandTo(this->meshes[i]->bbox);
        }
    } else {
        bbox.initTo(this->location);
    }
    
    for (const SceneObject* obj : this->objects) {
        bbox.expandTo(obj->getBoundingBox());
    }
    
    Matrix4 mw;
    getWorldTransform(&mw);
    bbox *= mw;
    
    //	printf("obj bbox = (%f, %f, %f) - (%f, %f, %f)\n", bbox.min.x, bbox.min.y, bbox.min.z, bbox.max.x, bbox.max.y, bbox.max.z);
    //	printf("size     = (%f, %f, %f)\n\n", bbox.size.x, bbox.size.y, bbox.size.z);
    
    return bbox;
}

SceneObject* SceneObject::clone() const {
    SceneObject* obj = new SceneObject();
    
    obj->setName(this->name);
    obj->material = this->material;
    
    for (Mesh* mesh : this->meshes) {
        obj->addMesh(*mesh);
    }
    
    for (SceneObject* child : this->objects) {
        SceneObject* childClone = child->clone();
        obj->addObject(*childClone);
    }
    
    return obj;
}

/////////////////// Scene ///////////////////

const std::vector<SceneObject*>& Scene::getObjects() const {
    return this->objects;
}

std::vector<SceneObject*>& Scene::getObjects() {
    return this->objects;
}

void Scene::addObject(SceneObject& object) {
    this->objects.push_back(&object);
}

void Scene::removeObject(SceneObject& object) {
    const auto& it = std::find(this->objects.begin(), this->objects.end(), &object);
    if (it != this->objects.end()) {
        this->objects.erase(it);
    }
    object.setParent(NULL);
}

void Scene::clearObjects() {
    this->objects.clear();
}

Scene::~Scene() {
    for (SceneObject* obj : this->objects) {
        delete obj;
    }

    this->objects.clear();
}

void Scene::buildEnvmapCDF() {
    this->envmapMarginalY.clear();
    this->envmapConditionalX.clear();
    this->envmapTotalWeight = 0.0f;
    this->envmapW = 0;
    this->envmapH = 0;

    this->envCubeMarginalFace.clear();
    this->envCubeMarginalY.clear();
    this->envCubeConditionalX.clear();
    this->envCubeTotalWeight = 0.0f;
    this->envCubeFaceSize = 0;

    // Cubemap path: when all six faces are attached, build a luminance CDF
    // per face weighted by each texel's solid angle on the unit sphere. This
    // matches the equirect CDF's convention (pdf in solid-angle units) so the
    // MIS weights interoperate.
    bool hasCube = true;
    for (int i = 0; i < 6; i++) if (this->envCubemapFaces[i] == NULL) { hasCube = false; break; }
    if (hasCube) {
        const int W = (int)this->envCubemapFaces[0]->getImage().width();
        const int H = (int)this->envCubemapFaces[0]->getImage().height();
        if (W > 0 && H > 0) {
            this->envCubeFaceSize = W;  // assumes square faces, consistent across all 6
            this->envCubeMarginalFace.assign(7, 0.0f);
            this->envCubeMarginalY.assign((size_t)6 * (H + 1), 0.0f);
            this->envCubeConditionalX.assign((size_t)6 * H * (W + 1), 0.0f);

            float faceWeights[6] = { 0, 0, 0, 0, 0, 0 };

            for (int f = 0; f < 6; f++) {
                const auto& img = this->envCubemapFaces[f]->getImage();
                std::vector<float> rowWeight(H, 0.0f);
                for (int y = 0; y < H; y++) {
                    this->envCubeConditionalX[((size_t)f * H + y) * (W + 1)] = 0.0f;
                    float rowSum = 0.0f;
                    const float t = 1.0f - (y + 0.5f) * (2.0f / (float)H);  // face-local, [-1, 1]
                    for (int x = 0; x < W; x++) {
                        const float s = (x + 0.5f) * (2.0f / (float)W) - 1.0f;
                        // Per-texel solid angle on the unit sphere ∝ 1/(s² + t² + 1)^(3/2).
                        const float denom = s * s + t * t + 1.0f;
                        const float jac = 1.0f / (denom * sqrtf(denom));
                        const color4f c = img.getPixel(x, y);
                        const float lum = fmaxf(0.0f, 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
                        const float w = lum * jac;
                        rowSum += w;
                        this->envCubeConditionalX[((size_t)f * H + y) * (W + 1) + (x + 1)] = rowSum;
                    }
                    if (rowSum > 0.0f) {
                        const float inv = 1.0f / rowSum;
                        for (int x = 1; x <= W; x++) {
                            this->envCubeConditionalX[((size_t)f * H + y) * (W + 1) + x] *= inv;
                        }
                    }
                    rowWeight[y] = rowSum;
                }
                this->envCubeMarginalY[(size_t)f * (H + 1)] = 0.0f;
                for (int y = 0; y < H; y++) {
                    this->envCubeMarginalY[(size_t)f * (H + 1) + y + 1] =
                        this->envCubeMarginalY[(size_t)f * (H + 1) + y] + rowWeight[y];
                }
                const float faceTotal = this->envCubeMarginalY[(size_t)f * (H + 1) + H];
                faceWeights[f] = faceTotal;
                if (faceTotal > 0.0f) {
                    const float inv = 1.0f / faceTotal;
                    for (int y = 0; y <= H; y++) this->envCubeMarginalY[(size_t)f * (H + 1) + y] *= inv;
                }
            }

            float total = 0.0f;
            for (int f = 0; f < 6; f++) total += faceWeights[f];
            this->envCubeTotalWeight = total;
            if (total > 0.0f) {
                const float inv = 1.0f / total;
                float acc = 0.0f;
                this->envCubeMarginalFace[0] = 0.0f;
                for (int f = 0; f < 6; f++) {
                    acc += faceWeights[f];
                    this->envCubeMarginalFace[f + 1] = acc * inv;
                }
            }
        }
    }

    if (this->envmap == NULL) return;
    const auto& img = this->envmap->getImage();
    const int W = (int)img.width();
    const int H = (int)img.height();
    if (W <= 0 || H <= 0) return;

    this->envmapW = W;
    this->envmapH = H;

    // Row-major per-pixel weights, each weighted by sin(theta) so equal-area
    // samples come from a sphere, not from the image (pixels near the poles
    // of an equirectangular map are highly foreshortened).
    std::vector<float> rowWeight(H, 0.0f);
    this->envmapConditionalX.resize((size_t)H * (W + 1));

    for (int y = 0; y < H; y++) {
        const float v = (y + 0.5f) / (float)H;
        const float sinTheta = sinf((float)M_PI * v);
        float rowSum = 0.0f;
        this->envmapConditionalX[(size_t)y * (W + 1)] = 0.0f;
        for (int x = 0; x < W; x++) {
            const color4f c = img.getPixel(x, y);
            const float lum = fmaxf(0.0f, 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
            rowSum += lum * sinTheta;
            this->envmapConditionalX[(size_t)y * (W + 1) + (x + 1)] = rowSum;
        }
        rowWeight[y] = rowSum;
    }

    // Normalize conditional CDFs per row. Rows with zero total pick x=0 by
    // fallthrough; their pdf is zero and NEE just skips them.
    for (int y = 0; y < H; y++) {
        const float total = rowWeight[y];
        if (total > 0.0f) {
            const float inv = 1.0f / total;
            for (int x = 1; x <= W; x++) this->envmapConditionalX[(size_t)y * (W + 1) + x] *= inv;
        }
    }

    // Marginal CDF over rows.
    this->envmapMarginalY.resize(H + 1);
    this->envmapMarginalY[0] = 0.0f;
    for (int y = 0; y < H; y++) this->envmapMarginalY[y + 1] = this->envmapMarginalY[y] + rowWeight[y];

    this->envmapTotalWeight = this->envmapMarginalY[H];
    if (this->envmapTotalWeight > 0.0f) {
        const float inv = 1.0f / this->envmapTotalWeight;
        for (int y = 0; y <= H; y++) this->envmapMarginalY[y] *= inv;
    }
}

SceneObject* Scene::findObjectByName(const string& name) {
    for (SceneObject* obj : this->objects) {
        if (obj->getName() == name) {
            return obj;
        }
        
        SceneObject* child = obj->findObjectByName(name);
        if (child != NULL) {
            return child;
        }
    }
    
    return NULL;
}

void Scene::eachChild(std::function<bool(SceneObject*)> iterator) {
    for (SceneObject* child : this->objects) {
        if (!iterator(child)) return;
    }
    
    for (SceneObject* child : this->objects) {
        if (!child->eachChild(iterator)) return;
    }
}

void Scene::applyTransform() {
    Matrix4 mat;
    
    for (auto* obj : this->objects) {
        obj->applyTransform(mat);
    }
}

/////////////////// SceneResourcePool ///////////////////

SceneResourcePool SceneResourcePool::instance;

SceneResourcePool::SceneResourcePool() {
    
}

SceneResourcePool::~SceneResourcePool() {
    for (const auto& it : this->meshes) {
        delete it.second;
    }
    
    for (const auto& set : this->textures) {
        delete set.second;
    }
    
    for (const auto& set : this->normalmaps) {
        delete set.second;
    }
    
    for (const auto& arkey : this->archives) {
        delete arkey.second;
    }
    
    this->meshes.clear();
    this->textures.clear();
    this->normalmaps.clear();
    this->archives.clear();
}

Mesh* SceneResourcePool::loadMeshFromFile(const string& meshURI, Archive* archive) {
    Mesh* mesh = new Mesh();

    if (meshURI.startsWith("sob://")
        || meshURI.startsWith("tob://")) {
        char bundleName[PATH_MAX];
        char uidstr[12];
        
        if (meshURI.startsWith("tob")) {
            _sscanf(meshURI.getBuffer(), "tob://%[^/]/%s", bundleName, uidstr);
        } else if (meshURI.startsWith("sob")) {
            _sscanf(meshURI.getBuffer(), "sob://%[^/]/%s", bundleName, uidstr);
        }
        
        if (strnlen(bundleName, PATH_MAX) > 0) {
            Archive* meshar = NULL;
            
            if (strncmp(bundleName, "__this__", PATH_MAX) == 0) {
                meshar = archive;
            } else {
                const auto arp = this->archives.find(bundleName);
                if (arp != this->archives.end()) {
                    meshar = arp->second;
                }
            }
            
            if (meshar != NULL) {
                uint uid = (uint)std::stoul(uidstr, nullptr, 16);
                MeshLoader::load(*mesh, *meshar, uid);
            }
        }
    } else {
        
#if defined(FBX_SUPPORT)
        if (meshURI.endsWith(".fbx", StringComparingFlags::SCF_CASE_INSENSITIVE)) {
            SceneFBXLoader loader;
            // TODO
            //			loader.loadAsChildren(&obj, finalPath);
        } else {
            MeshLoader::load(*mesh, meshURI);
        }
#else
        
        if (meshURI.endsWith(".obj", StringComparingFlags::SCF_CASE_INSENSITIVE)) {
            ObjFileReader loader;
            loader.read(meshURI);
            const std::vector<ObjObject*> objObjs = loader.getObjects();
        } else {
            MeshLoader::load(*mesh, meshURI);
        }
#endif /* FBX_SUPPORT */
    }
    
    return mesh;
}

//Mesh* SceneResourcePool::getMesh(const string& meshURL) {
//	const auto p = this->meshes.find(meshURL);
//	
//	if (p == this->meshes.end()) {
//		Mesh* mesh = this->loadMeshFromFile(meshURL, NULL);
//		this->meshes[meshURL] = mesh;
//		return mesh;
//	} else {
//		return p->second;
//	}
//}

Texture* SceneResourcePool::getTexture(const string& path, Archive* bundle) {
    const auto it = this->textures.find(path);
    
    if (it != this->textures.end()) {
        return it->second;
    }
    
    Texture* tex = new Texture();
    
    if (path.startsWith("sob://")
        || path.startsWith("tob://")) {
        char bundleName[PATH_MAX];
        char uidstr[12];
        
        if (path.startsWith("tob://")) {
            _sscanf(path.getBuffer(), "tob://%[^/]/%s", bundleName, uidstr);
        } else if (path.startsWith("sob://")) {
            _sscanf(path.getBuffer(), "sob://%[^/]/%s", bundleName, uidstr);
        } else {
            throw Exception("illegal resource path in bundle");
        }
        
        if (strnlen(bundleName, PATH_MAX) > 0 && strnlen(uidstr, 12) > 0) {
            Archive* targetBundle = NULL;
            
            if (strncmp(bundleName, "__this__", PATH_MAX) == 0) {
                targetBundle = bundle;
            } else {
                const auto arp = this->archives.find(bundleName);
                if (arp != this->archives.end()) {
                    targetBundle = arp->second;
                }
            }
            
            if (targetBundle != NULL) {
                uint uid = (uint)std::stoul(uidstr, nullptr, 16);

                loadImage(tex->getImage(), *targetBundle, uid);
            }
        }

        // A bundle without the referenced image (or a failed decode) leaves
        // the Texture with a 0×0 Image. Sampling that produces a modulo-0
        // division and an out-of-bounds getPixel; drop the texture so the
        // material falls back to its flat colour instead.
        if (tex != NULL && (tex->getImage().width() == 0 || tex->getImage().height() == 0)) {
            delete tex;
            tex = NULL;
        }
    } else {
        
        //#if _WIN32
        //	path.replace('/', '\\');
        //#endif // _WIN32
        
        if (!tex->loadFromFile(path)) {
            delete tex;
            tex = NULL;
        }
    }
    
    if (tex != NULL) {
        this->textures[path] = tex;
    }
    
    return tex;
}

Archive* SceneResourcePool::loadArchive(const string &path) {
    return this->loadArchive(path, path);
}

Archive* SceneResourcePool::loadArchive(const string& name, const string& path) {
    
    const auto ar = this->archives.find(name);
    if (ar != this->archives.end()) {
        return ar->second;
    }
    
    Archive* archive = new Archive();
    
    //#if _WIN32
    //	finalPath.replace('/', '\\');
    //#endif // _WIN32
    
    try {
        archive->load(path);
    } catch (const void* e) {
#if defined(DEBUG) || defined(_DEBUG) || defined(DEBUG_LOCAL)
        printf("error load bundle: %s\n", path.getBuffer());
#endif /* DEBUG */
        return NULL;
    }
    
    this->archives[name] = archive;
    
    return archive;
}

//void SceneResourcePool::setArchive(const string& name, Archive* archive) {
//	this->archives[name] = archive;
//}

void SceneResourcePool::getAvailableMaterialName(string &name) {
    string tmpName = name;
    int index = 2;
    
    while (true) {
        const auto it = this->materials.find(tmpName);
        if (it == this->materials.end()) {
            break;
        }
        tmpName = name;
        tmpName.appendFormat("_%d", index++);
    }
    
    name = tmpName;
}

void SceneResourcePool::collect(const SceneObject& obj) {
    // material
    if (obj.material != Material()) {
        bool foundMat = false;
        
        for (auto& it : this->materials) {
            if (*it.second == obj.material) {
                foundMat = true;
            }
        }
        
        if (!foundMat) {
            string matname = obj.material.name;
            if (matname.isEmpty()) matname = obj.getName();
            if (matname.isEmpty()) matname = "mat";
            this->getAvailableMaterialName(matname);
            this->materials[matname] = &obj.material;
        }
    }
    
    // meshes
    for (Mesh* mesh : obj.meshes) {
        bool foundMesh = false;
        
        for (auto& it : this->meshes) {
            if (it.second == mesh) {
                foundMesh = true;
            }
        }
        
        if (!foundMesh) {
            string name;
            name.appendFormat("mesh%d", this->meshes.size() + 1);
            this->meshes[name] = mesh;
        }
        
        //		// lightmap
        //		if (mesh->hasLightmap && mesh->lightmap != NULL) {
        //			Texture* lmtex = mesh->lightmap;
        //
        //			bool foundTex = false;
        //
        //			for (auto& it : this->textures) {
        //				if (it.second == lmtex) {
        //					foundTex = true;
        //				}
        //			}
        //
        //			if (!foundTex) {
        //				string name;
        //				name.appendFormat("lmap%d", this->textures.size() + 1);
        //				this->textures[name] = lmtex;
        //			}
        //		}
        
    }
    
    // children
    for (const SceneObject* child : obj.objects) {
        if (child != NULL) {
            this->collect(*child);
        }
    }
}

void SceneResourcePool::clear() {
    this->meshes.clear();
    this->materials.clear();
    this->textures.clear();
    this->normalmaps.clear();
    this->archives.clear();
}

ReflectionMapObject::ReflectionMapObject() {
    this->visible = false;
}

}
