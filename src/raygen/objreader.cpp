///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include <string>
#include <algorithm>

#include "objreader.h"
#include "ucm/file.h"

#if _WIN32
#define _sscanf sscanf_s
#define _sprintf sprintf_s
#else
#define _sscanf sscanf
#define _sprintf sprintf
#endif // _WIN32

namespace raygen {

inline void str_copy_rlen(char* output, const char* input, const int len) {
    const int clen = (int)(size_t)strlen(input) - len;
#if _WIN32
    strncpy_s(output, clen + 1, input + len + 1, clen);
#else
    strncpy(output, input + len + 1, clen);
#endif // _WIN32
    output[clen + 1] = '\0';
}

void ObjMaterial::reset() {
    this->name.clear();
    this->textureFilename.clear();
    this->normalmapFilename.clear();
    ambient = color3(0, 0, 0);
    diffuse = color3(0, 0, 0);
    specular = color3(0, 0, 0);
    shininess = 0;
}

ObjObject* ObjObject::findChildrenByName(const string& name) {
    for (auto* obj : this->children) {
        if (obj->name.equals(name)) {
            return obj;
        }
    }
    
    for (auto* obj : this->children) {
        auto* child = obj->findChildrenByName(name);
        if (child != NULL) return child;
    }
    
    return NULL;
}

ObjFileReader::~ObjFileReader() {
    if (this->file != NULL) {
        delete file;
        this->file = NULL;
    }
    
    if (this->stream != NULL) {
        this->stream = NULL;
    }
    
    for (ObjObject* obj : this->rootObjects) {
        delete obj;
    }
    
    this->rootObjects.clear();
    
    for (ObjObject* obj : this->errorObjects) {
        delete obj;
    }
    
    this->errorObjects.clear();
    
    if (this->currentObject != NULL) {
        delete this->currentObject;
        this->currentObject = NULL;
    }
}

void ObjFileReader::read(const char* filename) {
    this->file = new File(filename);
    
    FileStream stream(filename);
    this->stream = &stream;
    stream.openRead(FileStreamType::Text);
    
    if (stream.error()) return;
    
    this->currentObject = new ObjObject();
    
    bool inheadCommentBlock = true;
    
    while (nextLine())
    {
        if (inheadCommentBlock && this->isLine("#")) {
            if (strstr(this->line, "3ds Max") != NULL
                || strstr(this->line, "uses millimeters as units") != NULL) {
                this->globleAutoScale = true;
                
                if (this->console != NULL) {
                    this->console->info("automatically scale meshes from millimeters\n");
                }
                
                inheadCommentBlock = false;
                continue;
            }
        } else {
            inheadCommentBlock = false;
        }
        
        if (this->isLine("v")) // vertex
        {
            vec3 vertex;
            _sscanf(line + 2, "%f %f %f", &vertex.x, &vertex.y, &vertex.z);
            
            if (this->globleAutoScale) {
                vertex *= 0.01f;
            }
            
            BoundingBox& bbox = this->bbox;
            
            if (this->firstVertex) {
                bbox.initTo(vertex);
                this->firstVertex = false;
            } else {
                bbox.expandTo(vertex);
            }
            
            readVertexs.push_back(vertex);
        }
        else if (this->isLine("vn")) // normal
        {
            vec3 normal;
            _sscanf(line + 3, "%f %f %f", &normal.x, &normal.y, &normal.z);
            readNormals.push_back(normal);
        }
        else if (this->isLine("vt")) // texcoord
        {
            texcoord uv;
            _sscanf(line + 3, "%f %f", &uv.u, &uv.v);
            readTexcoords.push_back(uv);
        }
        else if (this->isLine("f")) // triangle
        {
            if (this->currentObject->hasReadingError) {
                continue;
            }
            
            bool success = this->readSurfaceLine();
            
            if (!success) {
                if (!this->currentObject->hasReadingError) {
                    if (this->console != NULL) {
                        this->console->error("error: invalid surface data at line %d\n", this->lineNumber);
                    }
                    
                    if (this->stopOnError) {
                        return;
                    } else {
                        this->hasError = this->currentObject->hasReadingError = true;
                    }
                }
            }
        }
        else if (this->isLine("o")) // object start
        {
            this->finalizeObject();
            
            this->currentObject->name = this->line + 2;
            
            if (this->console != NULL) {
                this->console->trace("object %s\n", this->currentObject->name.getBuffer());
            }
        }
        else if (this->isLine("g")) {
            
            if (strcmp(this->line + 2, "default") == 0) continue;
            
            this->finalizeObject();
            
            if (this->console != NULL) {
                this->console->trace("group %s\n", this->line + 2);
            }
            
            string objectName;
            
            this->groupNameLexer.setInput(this->line + 2);
            
            while (!groupNameLexer.eof()) {
                if (!this->groupNameLexer.readIdentifier()) {
                    break;
                }
                
                const string groupName = this->groupNameLexer.getTokenInputString();
                
                this->currentObject->groupNames.push_back(groupName);
                
                if (objectName.length() > 0) {
                    objectName.append('_');
                }
                
                objectName.append(groupName);
            }
            
            this->currentObject->name = objectName;
            //			if (groups.size() > 0 && !groups.at(0).equals("default")) {
            //				this->finalizeObject();
            //
            //				if (groups.size() > 1) {
            //					const string& parentName = groups.at(groups.size() - 2);
            //					ObjObject* parent = findObjectByName(parentName);
            //					if (parent != NULL) {
            //						parent->children.push_back(this->currentObject);
            //						this->currentObject->parent = parent;
            //					} else {
            //						printf("!!!! parent not found !!!!\n");
            //					}
            //				}
            //
            //				this->currentObject->name = groups.at(groups.size() - 1);
            //
            //				printf("object %s\n", this->currentObject->name.getBuffer());
            //			}
        }
        else if (this->isLine("mtllib"))
        {
            // material file
            string matlibPath;
            
            if (file->getPath().length() > 0) {
                matlibPath.append(file->getPath().getBuffer(), file->getPath().length());
                matlibPath.append(PATH_SPLITTER);
            }
            
            matlibPath.append(this->line + 7);
            
            if (this->console != NULL) {
                this->console->info("reading %s...\n", matlibPath.getBuffer());
            }
            
            this->readMaterialLibrary(matlibPath);
        }
        else if (this->isLine("usemtl"))
        {
            if (this->currentObject != NULL) {
                string matName;
                matName.append(this->line + 7);
                
                const ObjMaterial* selectedMat = this->getMaterialByName(matName);
                
                if (selectedMat == NULL) {
                    this->currentObject->selectedMatName = matName;
                } else {
                    this->currentObject->setMaterial(selectedMat);
                }
            }
        }
    }
    
    stream.close();
    this->stream = NULL;
    
    this->finalizeObject();
    
    delete this->currentObject;
    this->currentObject = NULL;
    
    this->bbox.finalize();
}

bool ObjFileReader::readSurfaceLine()
{
    unsigned int vertexIndexes[4], texcoordIndexes[4], normalIndexes[4];
    
    ObjObject* obj = this->currentObject;
    
    auto& lexer = this->surfaceLineLexer;
    
    lexer.setInput(this->line + 2);
    lexer.enableSkipWS = false;
    
    int vertexCount = 0;
    
    bool hasNormal = false;
    bool hasTexcoord = false;
    
    while (!lexer.eof()) {
        
        if (!lexer.readNumber()) {
            return false;
        } else {
            vertexIndexes[vertexCount] = (uint)lexer.getCurrentToken().v_num - 1;
        }
        
        if (lexer.readChar('/')) {
            if (!lexer.readNumber()) {
                if (obj->hasTexcoord) {
                    if (this->console != NULL) {
                        this->console->error("error: object has different texcoord set\n");
                    }
                    return false;
                }
            }
            else {
                if (!obj->hasTexcoord && !this->firstObjectSurfaceData) {
                    if (this->console != NULL) {
                        this->console->error("error: object has different texcoord set\n");
                    }
                    return false;
                }
                texcoordIndexes[vertexCount] = (uint)lexer.getCurrentToken().v_num - 1;
                obj->hasTexcoord = hasTexcoord = true;
            }
        }
        
        if (lexer.readChar('/')) {
            if (!lexer.readNumber()) {
                if (obj->hasNormal) {
                    if (this->console != NULL) {
                        this->console->error("error: object has different normal set\n");
                    }
                    return false;
                }
            }
            else {
                if (!obj->hasNormal && !this->firstObjectSurfaceData) {
                    if (this->console != NULL) {
                        this->console->error("error: object has different normal set\n");
                    }
                    return false;
                }
                normalIndexes[vertexCount] = (uint)lexer.getCurrentToken().v_num - 1;
                obj->hasNormal = hasNormal = true;
            }
        }
        
        vertexCount++;
        
        if (!lexer.readChar(' ')) {
            break;
        }
    }
    
    if (vertexCount < 3) {
        if (this->console != NULL) {
            this->console->error("error: not enough numbers of vertex: %d\n", vertexCount);
        }
        return false;
    } else if (vertexCount > 3) {
        if (this->console != NULL) {
            this->console->error("error: surface must be triangulated\n");
        }
        return false;
    }
    
    const vec3& v1 = this->readVertexs[vertexIndexes[0]];
    const vec3& v2 = this->readVertexs[vertexIndexes[1]];
    const vec3& v3 = this->readVertexs[vertexIndexes[2]];
    
    //	vec3 n1, n2, n3, n4;
    //	vec2 uv1, uv2, uv3, uv4;
    vec3 n1, n2, n3;
    vec2 uv1, uv2, uv3;
    
    if (hasNormal) {
        n1 = this->readNormals[normalIndexes[0]];
        n2 = this->readNormals[normalIndexes[1]];
        n3 = this->readNormals[normalIndexes[2]];
    }
    
    if (hasTexcoord) {
        uv1 = this->readTexcoords[texcoordIndexes[0]];
        uv2 = this->readTexcoords[texcoordIndexes[1]];
        uv3 = this->readTexcoords[texcoordIndexes[2]];
    }
    
    if (vertexCount == 4) {
        const vec3& v4 = this->readVertexs[vertexIndexes[3]];
        const vec3& n4 = this->readNormals[normalIndexes[3]];
        const vec2& uv4 = this->readTexcoords[texcoordIndexes[3]];
        
        //		vec3 faceNormal = (n1 + n2 + n3 + n4) / 3.0f;
        //
        //		vec3 edge1 = v2 - v1;
        //		vec3 edge2 = v3 - v2;
        //		vec3 normal = cross(edge1, edge2);
        //
        //		if (dot(normal, faceNormal) < 0) {
        //			const vec3 tmp = v2;
        //			v2 = v3;
        //			v3 = tmp;
        //
        //			const vec3 tmp = n2;
        //			n2 = n3;
        //			n3 = n2;
        //		}
        //
        //		vec3 edge2 = v2 - v3;
        //		vec3 edge3 = v3 - v2;
        //		vec3 normal = cross(edge1, edge2);
        //
        //		if (dot(normal, faceNormal) < 0) {
        //			const vec3 tmp = v2;
        //			v2 = v3;
        //			v3 = tmp;
        //
        //			const vec3 tmp = n2;
        //			n2 = n3;
        //			n3 = n2;
        //		}
        
        obj->vertices.push_back(v4);
        obj->vertices.push_back(v1);
        obj->vertices.push_back(v2);
        
        if (hasNormal) {
            obj->normals.push_back(n4);
            obj->normals.push_back(n1);
            obj->normals.push_back(n2);
        }
        
        if (hasTexcoord) {
            obj->texcoords.push_back(uv4);
            obj->texcoords.push_back(uv1);
            obj->texcoords.push_back(uv2);
        }
        
        obj->vertices.push_back(v4);
        obj->vertices.push_back(v2);
        obj->vertices.push_back(v3);
        
        if (hasNormal) {
            obj->normals.push_back(n4);
            obj->normals.push_back(n2);
            obj->normals.push_back(n3);
        }
        
        if (hasTexcoord) {
            obj->texcoords.push_back(uv4);
            obj->texcoords.push_back(uv2);
            obj->texcoords.push_back(uv3);
        }
        
    } else {
        
        obj->vertices.push_back(v1);
        obj->vertices.push_back(v2);
        obj->vertices.push_back(v3);
        
        if (hasNormal) {
            obj->normals.push_back(n1);
            obj->normals.push_back(n2);
            obj->normals.push_back(n3);
        }
        
        if (hasTexcoord) {
            obj->texcoords.push_back(uv1);
            obj->texcoords.push_back(uv2);
            obj->texcoords.push_back(uv3);
        }
    }
    
    this->firstObjectSurfaceData = false;
    
    return true;
}

void ObjFileReader::finalizeObject() {
    ObjObject* obj = this->currentObject;
    
    if (obj->hasReadingError) {
        this->markObjectError(*obj);
    } else if (obj->vertices.size() <= 0) {
        delete obj;
        obj = this->currentObject = NULL;
    } else {
        
        if (sameNameObjectAlreadyExist(obj->name)) {
            
            for (int index = 2;; index++) {
                string objName = obj->name;
                objName.appendFormat("_%d", index);
                
                if (!sameNameObjectAlreadyExist(objName)) {
                    obj->name = objName;
                    break;
                }
            }
        }
        
        createObjectMesh(*obj);
        
        if (obj->parent == NULL) {
            this->rootObjects.push_back(obj);
        }
    }
    
    this->currentObject = new ObjObject();
    this->firstObjectSurfaceData = true;
}

void ObjFileReader::createObjectMesh(ObjObject& obj) {
    
    uint vertexCount = (uint)obj.vertices.size();
    if (vertexCount <= 0 || obj.vertices.size() <= 0) return;
    
    Mesh& mesh = obj.getMesh();
    
    mesh.hasNormal = obj.hasNormal;
    mesh.hasTexcoord = obj.hasTexcoord;
    
    mesh.init(vertexCount);
    
    memcpy(mesh.vertices, obj.vertices.data(), mesh.vertexCount * sizeof(vec3));
    
    if (mesh.hasNormal && obj.normals.size() > 0) {
        memcpy(mesh.normals, obj.normals.data(), mesh.vertexCount * sizeof(vec3));
    }
    
    if (mesh.hasTexcoord && obj.texcoords.size() > 0) {
        memcpy(mesh.texcoords, obj.texcoords.data(), mesh.vertexCount * sizeof(vec2));
    }
}

ObjObject* ObjFileReader::findObjectByName(const string& name) {
    for (auto* obj : this->rootObjects) {
        if (obj->name.equals(name)) {
            return obj;
        }
    }
    
    for (auto* obj : this->rootObjects) {
        auto* child = obj->findChildrenByName(name);
        if (child != NULL) return child;
    }
    
    return NULL;
}

bool ObjFileReader::sameNameObjectAlreadyExist(const string &name) {
    for (auto* obj : this->rootObjects) {
        if (obj->name.equals(name)) {
            return true;
        }
    }
    
    return false;
}

void ObjFileReader::markObjectError(ObjObject& obj) {
    obj.hasReadingError = true;
    
    if (std::find(errorObjects.begin(), errorObjects.end(), &obj) == errorObjects.end()) {
        this->errorObjects.push_back(&obj);
    }
}

void ObjFileReader::readMaterialLibrary(const string& matlibPath)
{
    FileStream fs(matlibPath);
    
    try {
        fs.openRead(FileStreamType::Text);
    } catch (const FileException&) {
        if (this->console != NULL) {
            this->console->warn("%s read failed\n", matlibPath.getBuffer());
        }
        return;
    }
    
    ObjMaterial mat;
    bool hasMat = false;
    
    while (fs.readLine(this->line, LINE_BUFFER_LENGTH))
    {
        if (isLine("newmtl"))
        {
            if (hasMat) {
                this->materials.push_back(mat);
            }
            
            mat.reset();
            mat.name.append(this->line + 7);
            
            if (this->console != NULL) {
                this->console->info("  newmat %s...\n", mat.name.getBuffer());
            }
            
            hasMat = true;
        }
        else if (isLine("Ns")) {
            float shininess;
            if (_sscanf(this->line + 3, "%f", &shininess)) {
                mat.setShininess(shininess);
            }
        }
        else if (isLine("Ka")) {
            color3 ambient;
            if (_sscanf(this->line + 3, "%f %f %f", &ambient.r, &ambient.g, &ambient.b)) {
                mat.setAmbient(ambient);
            }
        }
        else if (isLine("Kd")) {
            color3 diffuse;
            if (_sscanf(this->line + 3, "%f %f %f", &diffuse.r, &diffuse.g, &diffuse.b)) {
                mat.setDiffuse(diffuse);
            }
        }
        else if (isLine("Ks")) {
            color3 specular;
            if (_sscanf(this->line + 3, "%f %f %f", &specular.r, &specular.g, &specular.b)) {
                mat.setSpecular(specular);
            }
        }
        else if (isLine("Tf")) {
            color3 transparency;
            if (_sscanf(this->line + 3, "%f %f %f", &transparency.r, &transparency.g, &transparency.b)) {
                mat.transparency = 1.0f - (transparency.r + transparency.g + transparency.b) / 3.0f;
            }
        }
        else if (isLine("map_Kd")) {
            char textureFilename[300];
            str_copy_rlen(textureFilename, this->line, 6);
            
            mat.setTextureFilename(textureFilename);
        }
        else if (isLine("map_Bump")) {
            char normalmapFilename[300];
            float normalIntensity = 1;
            
            if (strncmp(this->line + 9, "-bm", 3) == 0) {
                _sscanf(this->line + 13, "%f %s", &normalIntensity, normalmapFilename);
            } else {
                str_copy_rlen(normalmapFilename, this->line, 8);
            }
            
            //			mat.setNormalmapIntensity(normalIntensity);
            mat.setNormalmapFilename(normalmapFilename);
        }
        else if (isLine("map_t_normal")) {
            char normalmapFilename[300];
            
            //			if (strncmp(this->line + 12, "-bm", 3) == 0) {
            //				float bmvalue;
            //				if (_sscanf(this->line + 12, "-bm %f %s", &bmvalue, normalmapFilename) >= 2) {
            //					mat.setNormalmapFilename(normalmapFilename);
            //				}
            //			} else {
            str_copy_rlen(normalmapFilename, this->line, 12);
            mat.setNormalmapFilename(normalmapFilename);
            //			}
        }
    }
    
    fs.close();
    
    if (hasMat) {
        this->materials.push_back(mat);
    }
    
}

const ObjMaterial* ObjFileReader::getMaterialByName(const string& name) const {
    
    for (const auto& mat : this->materials) {
        if (mat.name == name) {
            return &mat;
        }
    }
    
    return NULL;
}

}
