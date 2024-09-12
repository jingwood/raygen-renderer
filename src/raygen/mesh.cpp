///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "mesh.h"
#include <algorithm>

namespace raygen {

Mesh::~Mesh() {
    if (this->vertices != NULL) {
        delete [] this->vertices;
        this->vertices = NULL;
    }
    
    if (this->normals != NULL) {
        delete [] this->normals;
        this->normals = NULL;
    }
    
    if (this->texcoords != NULL) {
        delete [] this->texcoords;
        this->texcoords = NULL;
    }
    
    if (this->indexes != NULL) {
        delete [] this->indexes;
        this->indexes = NULL;
    }
    
    if (this->tangents != NULL) {
        delete [] this->tangents;
        this->tangents = NULL;
    }
    
    if (this->bitangents != NULL) {
        delete [] this->bitangents;
        this->bitangents = NULL;
    }
    
    if (this->colors != NULL) {
        delete [] this->colors;
        this->colors = NULL;
    }
    
    this->vertexCount = 0;
    this->uvCount = 0;
    this->indexCount = 0;
}

void Mesh::init(const uint vertexCount, const uint uvCount, const uint indexCount) {
    
    this->vertexCount = vertexCount;
    this->uvCount = uvCount;
    this->indexCount = indexCount;
    
    this->vertices = new vec3[vertexCount];
    
    if (this->hasNormal) {
        this->normals = new vec3[vertexCount];
    }
    
    if (this->hasTexcoord) {
        this->texcoords = new vec2[vertexCount * uvCount];
    }
    
    if (this->indexCount > 0) {
        this->indexes = new vertex_index_t[indexCount];
    }
    
    if (this->hasTangentSpaceBasis) {
        this->tangents = new vec3[this->vertexCount];
        this->bitangents = new vec3[this->vertexCount];
    }
    
    if (this->hasColor) {
        this->colors = new color3[this->vertexCount];
    }
}

void Mesh::getIndexes(const ulong triangleNumber, vertex_index_t& i1, vertex_index_t& i2, vertex_index_t& i3) const {
    ulong index = triangleNumber * 3;
    
    i1 = this->indexes[index + 0];
    i2 = this->indexes[index + 1];
    i3 = this->indexes[index + 2];
}

void Mesh::getVertex(const ulong triangleNumber, vec3* v1, vec3* v2, vec3* v3) const {
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleNumber, i1, i2, i3);
    }
    else {
        i1 = (vertex_index_t)triangleNumber * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    *v1 = this->vertices[i1];
    *v2 = this->vertices[i2];
    *v3 = this->vertices[i3];
}

void Mesh::setVertex(const ulong triangleIndex, const vec3& v1, const vec3& v2, const vec3& v3) {
    if (!this->hasTexcoord || this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    this->vertices[i1] = v1;
    this->vertices[i2] = v2;
    this->vertices[i3] = v3;
}

void Mesh::getNormal(const ulong triangleNumber, vec3* n1, vec3* n2, vec3* n3) const {
    if (!this->hasNormal || this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleNumber, i1, i2, i3);
    }
    else {
        i1 = (vertex_index_t)triangleNumber * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    *n1 = this->normals[i1];
    *n2 = this->normals[i2];
    *n3 = this->normals[i3];
}

void Mesh::setNormal(const ulong triangleIndex, const vec3& n1, const vec3& n2, const vec3& n3)
{
    if (!this->hasNormal || this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    this->normals[i1] = n1;
    this->normals[i2] = n2;
    this->normals[i3] = n3;
}

void Mesh::getUV(const uint uvIndex, const ulong triangleNumber, vec2* uv1, vec2* uv2, vec2* uv3) const {
    if (!this->hasTexcoord || this->vertexCount <= 0 || uvIndex >= this->uvCount) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleNumber, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleNumber * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    int offset = uvIndex * this->vertexCount;
    
    *uv1 = this->texcoords[offset + i1];
    *uv2 = this->texcoords[offset + i2];
    *uv3 = this->texcoords[offset + i3];
}

void Mesh::setUV(const uint uvIndex, const ulong triangleNumber, const vec2& uv1, const vec2& uv2, const vec2& uv3) {
    if (!this->hasTexcoord || this->vertexCount <= 0 || uvIndex >= this->uvCount) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleNumber, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleNumber * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    int offset = uvIndex * this->vertexCount;
    
    this->texcoords[offset + i1] = uv1;
    this->texcoords[offset + i2] = uv2;
    this->texcoords[offset + i3] = uv3;
}

void Mesh::setColor(const ulong triangleIndex, const color3& c1, const color3& c2, const color3& c3) {
    if (!this->hasColor || this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    this->colors[i1] = c1;
    this->colors[i2] = c2;
    this->colors[i3] = c3;
}

void Mesh::getTriangleNUV(const ulong triangleIndex, const uint uvIndex, TriangleNUV* tnuv) {
    
    if (this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    tnuv->v1 = this->vertices[i1];
    tnuv->v2 = this->vertices[i2];
    tnuv->v3 = this->vertices[i3];
    
    if (this->hasNormal) {
        tnuv->n1 = this->normals[i1];
        tnuv->n2 = this->normals[i2];
        tnuv->n3 = this->normals[i3];
    }
    
    if (this->hasTexcoord && uvIndex < this->uvCount) {
        int uvoffset = uvIndex * this->vertexCount;
        
        tnuv->uv1 = this->texcoords[uvoffset + i1];
        tnuv->uv2 = this->texcoords[uvoffset + i2];
        tnuv->uv3 = this->texcoords[uvoffset + i3];
    }
}

void Mesh::setTriangleNUV(const ulong triangleIndex, const uint uvIndex, const TriangleNUV& tnuv) {
    if (this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    this->vertices[i1] = tnuv.v1;
    this->vertices[i2] = tnuv.v2;
    this->vertices[i3] = tnuv.v3;
    
    if (this->hasNormal) {
        this->normals[i1] = tnuv.n1;
        this->normals[i2] = tnuv.n2;
        this->normals[i3] = tnuv.n3;
    }
    
    if (this->hasTexcoord && uvIndex < this->uvCount) {
        int uvoffset = uvIndex * this->vertexCount;
        
        this->texcoords[uvoffset + i1] = tnuv.uv1;
        this->texcoords[uvoffset + i2] = tnuv.uv2;
        this->texcoords[uvoffset + i3] = tnuv.uv3;
    }
}

void Mesh::getTriangleNUV2TBC(const ulong triangleIndex, TriangleNUV2TBC* tnuv) {
    
    if (this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    tnuv->v1 = this->vertices[i1];
    tnuv->v2 = this->vertices[i2];
    tnuv->v3 = this->vertices[i3];
    
    if (this->hasNormal) {
        tnuv->n1 = this->normals[i1];
        tnuv->n2 = this->normals[i2];
        tnuv->n3 = this->normals[i3];
    }
    
    if (this->hasTexcoord) {
        const int uv1offset = 0 * this->vertexCount;
        
        tnuv->uv1 = this->texcoords[uv1offset + i1];
        tnuv->uv2 = this->texcoords[uv1offset + i2];
        tnuv->uv3 = this->texcoords[uv1offset + i3];
        
        const int uv2offset = 1 * this->vertexCount;
        
        tnuv->uv4 = this->texcoords[uv2offset + i1];
        tnuv->uv5 = this->texcoords[uv2offset + i2];
        tnuv->uv6 = this->texcoords[uv2offset + i3];
    }
    
    if (this->hasColor) {
        tnuv->c1 = this->colors[i1];
        tnuv->c2 = this->colors[i2];
        tnuv->c3 = this->colors[i3];
    }
}

void Mesh::setTriangleNUV2TBC(const ulong triangleIndex, const TriangleNUV2TBC& tnuv) {
    if (this->vertexCount <= 0) return;
    
    vertex_index_t i1, i2, i3;
    
    if (this->indexCount > 0) {
        this->getIndexes(triangleIndex, i1, i2, i3);
    } else {
        i1 = (vertex_index_t)triangleIndex * 3;
        i2 = i1 + 1;
        i3 = i1 + 2;
    }
    
    //	memcpy(this->vertices + i1, tnuv.vs, sizeof(vec3) * 3);
    this->vertices[i1] = tnuv.v1;
    this->vertices[i2] = tnuv.v2;
    this->vertices[i3] = tnuv.v3;
    
    if (this->hasNormal) {
        //		memcpy(this->normals + i1, tnuv.ns, sizeof(vec3) * 3);
        this->normals[i1] = tnuv.n1;
        this->normals[i2] = tnuv.n2;
        this->normals[i3] = tnuv.n3;
    }
    
    // TODO
    //	if (this->hasTexcoord) {
    //		const int uv1offset = 0 * this->vertexCount;
    //
    //		this->texcoords[uv1offset + i1] = tnuv.uv1;
    //		this->texcoords[uv1offset + i2] = tnuv.uv2;
    //		this->texcoords[uv1offset + i3] = tnuv.uv3;
    //
    //		const int uv2offset = 1 * this->vertexCount;
    //
    //		this->texcoords[uv2offset + i1] = tnuv.uv4;
    //		this->texcoords[uv2offset + i2] = tnuv.uv5;
    //		this->texcoords[uv2offset + i3] = tnuv.uv6;
    //	}
    //
    //	if (this->hasTangentSpaceBasis) {
    //		this->tangents[i1] = tnuv.t1;
    //		this->tangents[i2] = tnuv.t2;
    //		this->tangents[i3] = tnuv.t3;
    //		this->bitangents[i1] = tnuv.b1;
    //		this->bitangents[i2] = tnuv.b2;
    //		this->bitangents[i3] = tnuv.b3;
    //	}
    //
    //	if (this->hasColor) {
    //		this->colors[i1] = tnuv.c1;
    //		this->colors[i2] = tnuv.c2;
    //		this->colors[i3] = tnuv.c3;
    //	}
}

ulong Mesh::calcMemorySize() const {
    return this->vertexCount * sizeof(vec3)
    + (this->hasNormal ? this->vertexCount * sizeof(vec3) : 0)
    + (this->hasTexcoord ? (this->uvCount * this->vertexCount * sizeof(vec2)) : 0)
    + this->indexCount * sizeof(vertex_index_t)
    + (this->hasTangentSpaceBasis ? (this->vertexCount * 2 * sizeof(vec3)) : 0);
}

void Mesh::resizeVertexCount(int newVertexCount) {
#define RESIZE_BUFFER(bufferName, typename, sets) \
typename* new_ ## bufferName = new typename[newVertexCount * sets]; \
if (this->bufferName != NULL) { \
delete [] this->bufferName; \
} \
this->bufferName = new_ ## bufferName;
    
    RESIZE_BUFFER(vertices, vec3, 1);
    
    if (this->hasNormal) {
        RESIZE_BUFFER(normals, vec3, 1);
    }
    
    if (this->hasTexcoord) {
        RESIZE_BUFFER(texcoords, vec2, this->uvCount);
    }
    
    if (this->hasTangentSpaceBasis) {
        RESIZE_BUFFER(tangents, vec3, 1);
        RESIZE_BUFFER(bitangents, vec3, 1);
    }
    
    if (this->hasColor) {
        RESIZE_BUFFER(colors, color3, 1);
    }
    
    this->vertexCount = newVertexCount;
#undef RESIZE_BUFFER
}

Mesh* Mesh::clone() const {
    Mesh* m = new Mesh();
    Mesh::copy(*this, *m);
    return m;
}

void Mesh::copy(const Mesh& m1, Mesh& m2) {
#define COPY_BUFFER(bufferName, typename, sets) \
if (m2.bufferName != NULL) { \
delete [] m2.bufferName; \
} \
m2.bufferName = new typename[m1.vertexCount * sets]; \
memcpy(m2.bufferName, m1.bufferName, sizeof(typename) * sets * m1.vertexCount);
    
    COPY_BUFFER(vertices, vec3, 1);
    
    if (m1.hasNormal) {
        COPY_BUFFER(normals, vec3, 1);
        m2.hasNormal = true;
    }
    
    if (m1.hasTexcoord) {
        COPY_BUFFER(texcoords, vec2, m1.uvCount);
        m2.uvCount = m1.uvCount;
        m2.hasTexcoord = true;
    }
    
    if (m1.hasTangentSpaceBasis) {
        COPY_BUFFER(tangents, vec3, 1);
        COPY_BUFFER(bitangents, vec3, 1);
        m2.hasTangentSpaceBasis = true;
    }
    
    if (m1.hasColor) {
        COPY_BUFFER(colors, color3, 1);
    }
    
    if (m1.hasGrabBoundary) {
        m2.grabBoundary = m1.grabBoundary;
        m2.hasGrabBoundary = m1.hasGrabBoundary;
    }
    
    if (m1.indexCount > 0) {
        if (m2.indexes != NULL) {
            delete [] m2.indexes;
        }
        m2.indexes = new vertex_index_t[m1.vertexCount];
        memcpy(m2.indexes, m1.indexes, sizeof(vertex_index_t) * m1.vertexCount);
        m2.indexCount = m1.indexCount;
    }
    
    if (m1.hasLightmap) {
        m2.lightmapTrunkUid = m1.lightmapTrunkUid;
        m2.hasLightmap = true;
    }
    
    if (m1.hasRefmap) {
        m2.refmapTrunkUid = m1.refmapTrunkUid;
        m2.hasRefmap = true;
    }
    
    m2.vertexCount = m1.vertexCount;
    m2.trunkUid = m1.trunkUid;
    m2.calcBoundingBox();
    
#undef COPY_BUFFER
    
}

void Mesh::inversePolygonVertexOrder(bool applyTexcoord) {
    const uint triangleCount = this->getTriangleCount();
    
    for (int i = 0; i < triangleCount; i++) {
        vec3 v1, v2, v3;
        this->getVertex(i, &v1, &v2, &v3);
        vec3 vtmp = v2;
        v2 = v3;
        v3 = vtmp;
        this->setVertex(i, v1, v2, v3);
        
        if (this->hasNormal) {
            vec3 n1, n2, n3;
            this->getNormal(i, &n1, &n2, &n3);
            vec3 ntmp = n2;
            n2 = n3;
            n3 = ntmp;
            this->setNormal(i, n1, n2, n3);
        }
        
        if (applyTexcoord && this->hasTexcoord) {
            for (int k = 0; k < this->uvCount; k++) {
                vec2 uv1, uv2, uv3;
                this->getUV(k, i, &uv1, &uv2, &uv3);
                vec2 uvtmp = uv2;
                uv2 = uv3;
                uv3 = uvtmp;
                this->setUV(k, i, uv1, uv2, uv3);
            }
        }
    }
}

void Mesh::calcNormals() {
    if (this->vertexCount <= 0) return;
    
    if (this->normals == NULL) {
        this->normals = new vec3[this->vertexCount];
    }
    
    for (uint i = 0; i < this->vertexCount; i += 3) {
        const vec3& v1 = this->vertices[i];
        const vec3& v2 = this->vertices[i + 1];
        const vec3& v3 = this->vertices[i + 2];
        
        vec3 n1 = normalize(cross(v1 - v3, v1 - v2));
        this->normals[i] = n1;
        this->normals[i + 1] = n1;
        this->normals[i + 2] = n1;
    }
    
    this->hasNormal = true;
}

void Mesh::flipNormals() {
    if (this->vertexCount <= 0 || !this->hasNormal || this->normals == NULL) {
        return;
    }
    
    for (int i = 0; i < this->vertexCount; i++) {
        vec3& n = this->normals[i];
        n = -n;
    }
}

void Mesh::extractIndex()
{
    if (this->indexCount <= 0 || this->indexes == NULL) {
        return;
    }
    
    // create buffers
    vec3* newVertices = new vec3[this->indexCount];
    vec3* newNormals = NULL;
    vec2* newTexcoords = NULL;
    vec3* newTangents = NULL;
    vec3* newBitangents = NULL;
    color3* newColors = NULL;
    
    if (this->hasNormal) {
        newNormals = new vec3[this->indexCount];
    }
    
    if (this->hasTangentSpaceBasis) {
        newTangents = new vec3[this->indexCount];
        newBitangents = new vec3[this->indexCount];
    }
    
    if (this->hasColor) {
        newColors = new color3[this->indexCount];
    }
    
    // expand indexes - vertices and normals
    for (uint i = 0; i < this->indexCount; i++) {
        const vertex_index_t index = this->indexes[i];
        
        newVertices[i] = this->vertices[index];
        
        if (this->hasNormal) {
            newNormals[i] = this->normals[index];
        }
        
        if (this->hasTangentSpaceBasis) {
            newTangents[i] = this->tangents[index];
            newBitangents[i] = this->bitangents[index];
        }
        
        if (this->hasColor) {
            newColors[i] = this->colors[index];
        }
    }
    
    // expand texcoords
    if (this->hasTexcoord) {
        newTexcoords = new vec2[this->indexCount * this->uvCount];
        
        for (int k = 0; k < this->uvCount; k++) {
            for (uint i = 0; i < this->indexCount; i++) {
                
                const vertex_index_t index = this->indexes[i];
                newTexcoords[this->indexCount * k + i] = this->texcoords[this->vertexCount * k + index];
            }
        }
    }
    
    // release old buffers
    delete [] this->vertices;
    this->vertices = newVertices;
    this->vertexCount = this->indexCount;
    
    if (this->hasNormal) {
        delete [] this->normals;
        this->normals = newNormals;
    }
    
    if (this->hasTexcoord) {
        delete [] this->texcoords;
        this->texcoords = newTexcoords;
    }
    
    if (this->hasTangentSpaceBasis) {
        delete [] this->tangents;
        delete [] this->bitangents;
        this->tangents = newTangents;
        this->bitangents = newBitangents;
    }
    
    if (this->hasColor) {
        delete [] this->colors;
        this->colors = newColors;
    }
    
    // release index buffer
    delete [] this->indexes;
    this->indexes = NULL;
    this->indexCount = 0;
}

void Mesh::composeIndex() {
    if (this->indexCount > 0 || this->indexes != NULL || this->vertexCount <= 0) {
        return;
    }
    
    std::vector<vec3> newVertices;
    std::vector<vec3> newNormals;
    std::vector<vec2> newTexcoords1;
    std::vector<vec2> newTexcoords2;
    std::vector<vec3> newTangents;
    std::vector<vec3> newBitangents;
    
    this->indexCount = this->vertexCount;
    this->indexes = new vertex_index_t[this->indexCount];
    
    for (uint i = 0; i < this->vertexCount; i++) {
        const vec3& v = this->vertices[i];
        
        vec3 n, tv, bv;
        vec2 uv1, uv2;
        
        if (this->hasNormal) {
            n = this->normals[i];
        }
        
        if (this->hasTexcoord) {
            uv1 = this->texcoords[i];
            
            if (this->uvCount > 1) {
                uv2 = this->texcoords[i + this->vertexCount];
            }
        }
        
        if (this->hasTangentSpaceBasis) {
            tv = this->tangents[i];
            bv = this->bitangents[i];
        }
        
        uint index = -1;
        
        for (uint k = 0; k < newVertices.size(); k++) {
            if (newVertices.at(k) == v
                && (!hasNormal || newNormals.at(k) == n)
                && (!hasTexcoord || (newTexcoords1.at(k) == uv1 && (this->uvCount <= 1 || newTexcoords2.at(k) == uv2)))
                && (!hasTangentSpaceBasis || (newTangents.at(k) == tv && newBitangents.at(k) == bv))
                ) {
                index = k;
                break;
            }
        }
        
        if (index == -1) {
            index = (uint)newVertices.size();
            
            newVertices.push_back(v);
            
            if (this->hasNormal) {
                newNormals.push_back(n);
            }
            
            if (this->hasTexcoord) {
                newTexcoords1.push_back(uv1);
                
                if (this->uvCount > 1) {
                    newTexcoords2.push_back(uv2);
                }
            }
            
            if (this->hasTangentSpaceBasis) {
                newTangents.push_back(tv);
                newBitangents.push_back(bv);
            }
        }
        
        this->indexes[i] = index;
    }
    
    // rearrangement vertex data
    
    if (this->vertices != NULL) {
        delete [] this->vertices;
    }
    
    this->vertexCount = (uint)newVertices.size();
    
    this->vertices = new vec3[this->vertexCount];
    memcpy(this->vertices, newVertices.data(), this->vertexCount * sizeof(vec3));
    
    if (this->normals != NULL) {
        delete [] this->normals;
        this->normals = NULL;
    }
    
    if (this->hasNormal) {
        this->normals = new vec3[this->vertexCount];
        memcpy(this->normals, newNormals.data(), this->vertexCount * sizeof(vec3));
        
        this->hasNormal = true;
    }
    
    if (this->texcoords != NULL) {
        delete [] this->texcoords;
        this->texcoords = NULL;
    }
    
    if (this->hasTexcoord) {
        this->texcoords = new vec2[this->vertexCount * this->uvCount];
        memcpy(this->texcoords, newTexcoords1.data(), this->vertexCount * sizeof(vec2));
        
        if (this->uvCount > 1) {
            memcpy(this->texcoords + this->vertexCount, newTexcoords2.data(), this->vertexCount * sizeof(vec2));
        }
        
        this->hasTexcoord = true;
    }
    
    if (this->tangents != NULL) {
        delete [] this->tangents;
        this->tangents = NULL;
    }
    
    if (this->bitangents != NULL) {
        delete [] this->bitangents;
        this->bitangents = NULL;
    }
    
    if (this->hasTangentSpaceBasis) {
        this->tangents = new vec3[this->vertexCount];
        memcpy(this->tangents, newTangents.data(), this->vertexCount * sizeof(vec3));
        
        this->bitangents = new vec3[this->vertexCount];
        memcpy(this->bitangents, newBitangents.data(), this->vertexCount * sizeof(vec3));
        
        this->hasTangentSpaceBasis = true;
    }
}

void Mesh::applyTransform(const Matrix4& m) {
    for (uint i = 0; i < this->vertexCount; i++) {
        
        vec3& v = this->vertices[i];
        v = (vec4(v, 1.0f) * m).xyz;
        
        if (this->hasNormal) {
            vec3& n = this->normals[i];
            n = (vec4(n, 0.0f) * m).xyz.normalize();
        }
    }
}

void Mesh::offset(const vec3 &off) {
    for (uint i = 0; i < this->vertexCount; i++) {
        vec3& v = this->vertices[i];
        v += off;
    }
}

vec3 Mesh::alignToOrigin() {
    if (this->vertexCount <= 0) return vec3::zero;
    
    if (!this->hasBoundingBox) {
        this->calcBoundingBox();
    }
    
    const vec3 center = this->bbox.min + (this->bbox.max - this->bbox.min) * 0.5f;
    
    if (center.equals(vec3::zero)) return vec3::zero;
    
    for (uint i = 0; i < this->vertexCount; i++) {
        vec3& v = this->vertices[i];
        v -= center;
    }
    
    return center;
}

void Mesh::inverseTexcoordV() {
    if (this->uvCount <= 0 || !this->hasTexcoord) return;
    
    for (uint i = 0; i < this->vertexCount; i++) {
        vec2& tex = this->texcoords[i];
        tex.v = 1.0f - tex.v;
    }
}

BoundingBox Mesh::calcBoundingBox() {
    if (this->vertexCount <= 0) return BoundingBox();
    
    BoundingBox& bbox = this->bbox;
    
    bbox.min = this->vertices[0];
    bbox.max = bbox.min;
    
    for (uint i = 1; i < this->vertexCount; i++) {
        const vec3& v = this->vertices[i];
        
        if (bbox.minX > v.x) bbox.minX = v.x;
        if (bbox.minY > v.y) bbox.minY = v.y;
        if (bbox.minZ > v.z) bbox.minZ = v.z;
        if (bbox.maxX < v.x) bbox.maxX = v.x;
        if (bbox.maxY < v.y) bbox.maxY = v.y;
        if (bbox.maxZ < v.z) bbox.maxZ = v.z;
    }
    
    bbox.finalize();
    
    this->hasBoundingBox = true;
    
    return this->bbox;
}

BoundingBox Mesh::getBoundingBox() {
    if (!this->hasBoundingBox) {
        this->calcBoundingBox();
    }
    
    return this->bbox;
}

vec2* Mesh::appendUVBuffer(const uint newCount) {
    if (newCount == 0) {
        throw Exception("newCount must be larger than 0");
    }
    
    const int bufferLen = this->vertexCount * (this->uvCount + newCount);
    vec2* newTexcoords = new vec2[bufferLen];
    memset(newTexcoords, 0, sizeof(vec2) * bufferLen);
    
    if (this->texcoords != NULL) {
        memcpy(newTexcoords, this->texcoords, this->vertexCount * sizeof(vec2) * this->uvCount);
        delete [] this->texcoords;
    }
    
    this->texcoords = newTexcoords;
    this->uvCount += newCount;
    
    this->hasTexcoord = true;
    
    return this->getUVBuffer(this->uvCount - newCount);
}

vec2* Mesh::getUVBuffer(const int index) {
    return this->texcoords + (this->vertexCount * index);
}

const vec2* Mesh::getUVBuffer(const int index) const {
    return this->texcoords + (this->vertexCount * index);
}

bool Mesh::hasUVBuffer(const int index) const {
    return this->uvCount > index;
}

void Mesh::generateUV2FromUV1() {
    if (!this->hasTexcoord || this->uvCount > 1 || this->vertexCount <= 0) return;
    
    vec2* texcoords2 = this->appendUVBuffer(1);
    
    this->normalizeUV(texcoords2, this->texcoords, this->vertexCount);
}

void Mesh::subdivideTriangles(const float minLength) {
    int triangleCount = this->getTriangleCount();
    
    std::vector<TriangleNUV2TBC> triangles;
    triangles.reserve(triangleCount);
    
    TriangleNUV2TBC t;
    
    for (int i = 0; i < triangleCount; i++) {
        this->getTriangleNUV2TBC(i, &t);
        triangles.push_back(t);
    }
    
    //std::vector<TriangleNUV2TBC> newTriangles;
    
    TriangleNUV2TBC ts[2];
    
    //	bool found = true;
    
    //	do {
    //		found = false;
    
    for (int i = 0; i < triangleCount; ) {
        
        bool found = this->subdivideTriangle(triangles, i, minLength, ts);
        
        if (found) {
            triangles[i] = ts[0];
            triangles.push_back(ts[1]);
            triangleCount++;
            
            //				found = true;
        } else {
            i++;
        }
    }
    //	} while (found);
    
    const int newTriangleCount = (int)triangles.size();
    
    this->resizeVertexCount(newTriangleCount * 3);
    
    for (int i = 0; i < newTriangleCount; i++) {
        this->setTriangleNUV2TBC(i, triangles[i]);
    }
    
}

bool Mesh::subdivideTriangle(const std::vector<TriangleNUV2TBC>& triangles, const int triangleIndex,
                             const float minLength, TriangleNUV2TBC ts[2]) {
    const TriangleNUV2TBC& t = triangles[triangleIndex];
    
    vec3 v1 = t.v1, v2 = t.v2, v3 = t.v3;
    vec3 n1 = t.n1, n2 = t.n2, n3 = t.n3;
    
    vec3 vedge21 = v2 - v1;
    vec3 vedge31 = v3 - v1;
    vec3 vedge32 = v3 - v2;
    vec3 nedge21 = n2 - n1;
    vec3 nedge31 = n3 - n1;
    vec3 nedge32 = n3 - n2;
    
    const float l21 = vedge21.length();
    const float l31 = vedge31.length();
    const float l32 = vedge32.length();
    
    if (l21 > l31 && l21 > l32) {
        if (l21 <= minLength) {
            return false;
        }
        
        vec3 vc21 = v1 + vedge21 * 0.5f;
        
        ts[0].v1 = v1; ts[0].v2 = vc21; ts[0].v3 = v3;
        ts[1].v1 = vc21; ts[1].v2 = v2; ts[1].v3 = v3;
        
        vec3 nc21 = n1 + nedge21 * 0.5f;
        
        ts[0].n1 = n1; ts[0].n2 = nc21; ts[0].n3 = n3;
        ts[1].n1 = nc21; ts[1].n2 = n2; ts[1].n3 = n3;
    }
    else if (l31 > l21 && l31 > l32) {
        if (l31 <= minLength) {
            return false;
        }
        
        vec3 vc31 = v1 + vedge31 * 0.5f;
        
        ts[0].v1 = v1; ts[0].v2 = v2; ts[0].v3 = vc31;
        ts[1].v1 = vc31; ts[1].v2 = v2; ts[1].v3 = v3;
        
        vec3 nc31 = n1 + nedge31 * 0.5f;
        
        ts[0].n1 = n1; ts[0].n2 = n2; ts[0].n3 = nc31;
        ts[1].n1 = nc31; ts[1].n2 = n2; ts[1].n3 = n3;
    }
    else {
        if (l32 <= minLength) {
            return false;
        }
        
        const vec3 vc32 = v2 + vedge32 * 0.5f;
        
        ts[0].v1 = v1; ts[0].v2 = v2; ts[0].v3 = vc32;
        ts[1].v1 = v1; ts[1].v2 = vc32; ts[1].v3 = v3;
        
        const vec3 nc32 = n2 + nedge32 * 0.5f;
        
        ts[0].n1 = n1; ts[0].n2 = n2; ts[0].n3 = nc32;
        ts[1].n1 = n1; ts[1].n2 = nc32; ts[1].n3 = n3;
    }
    
    return true;
}

void Mesh::grabVertices(const GrabBoundary& offset) {
    
    BoundingBox bbox = this->getBoundingBox();
    
    constexpr float epsilon = 0.001;
    const vec3 EPSILON_V = vec3(epsilon);
    
#define OFFSET_VERTEX(dir, axis) if (dir.contains(v)) v.axis += offset.dir;
#define GRAB_VERTICES(dir1, dir2, axis) 	for (int i = 0; i < this->vertexCount; i++) { \
vec3& v = this->vertices[i]; \
OFFSET_VERTEX(dir1, axis); \
OFFSET_VERTEX(dir2, axis); \
} \
bbox = this->calcBoundingBox(); \

    BoundingBox left = BoundingBox(bbox.min - EPSILON_V, vec3(bbox.min.x + this->grabBoundary.left, bbox.max.y, bbox.max.z) + EPSILON_V);
    BoundingBox right = BoundingBox(vec3(bbox.max.x - this->grabBoundary.right, bbox.min.y, bbox.min.z) - EPSILON_V, bbox.max + EPSILON_V);
    
    GRAB_VERTICES(left, right, x);
    
    BoundingBox back = BoundingBox(bbox.min - EPSILON_V, vec3(bbox.max.x, bbox.max.y, bbox.min.z + this->grabBoundary.back) + EPSILON_V);
    BoundingBox front = BoundingBox(vec3(bbox.min.x, bbox.min.y, bbox.max.z - this->grabBoundary.front) - EPSILON_V, bbox.max + EPSILON_V);
    
    GRAB_VERTICES(back, front, z);
    
    //	BoundingBox bottom = BoundingBox(bbox.min - EPSILON_V, vec3(bbox.min.x, bbox.max.y + this->grabBoundary.bottom, bbox.max.z) + EPSILON_V);
    //	BoundingBox top = BoundingBox(vec3(bbox.max.x, bbox.min.y - this->grabBoundary.top, bbox.min.z) - EPSILON_V, bbox.max + EPSILON_V);
    //
    //	GRAB_VERTICES(bottom, top, y);
    
#undef OFFSET_VERTEX
#undef GRAB_VERTICES
}

void Mesh::grabResize(const GrabBoundary& newSize) {
    const BoundingBox bbox = this->getBoundingBox();
    
    GrabBoundary offset;
    offset.left = newSize.left - bbox.min.x;
    offset.right = newSize.right - bbox.max.x;
    offset.top = newSize.top - bbox.max.y;
    offset.bottom = newSize.bottom - bbox.min.y;
    
    if (newSize.front != 0) offset.front = newSize.front - bbox.max.z;
    if (newSize.back != 0) offset.back = newSize.back - bbox.min.z;
    
    this->grabVertices(offset);
}

void Mesh::normalizeUV(vec2* uvdest, const vec2* uvsrc, const int count) {
    vec2 min = uvsrc[0], max = uvsrc[0];
    
    for (int i = 1; i < count; i++) {
        const vec2& uv1 = uvsrc[i];
        
        if (min.u > uv1.u) min.u = uv1.u;
        if (min.v > uv1.v) min.v = uv1.v;
        if (max.u < uv1.u) max.u = uv1.u;
        if (max.v < uv1.v) max.v = uv1.v;
    }
    
    float sx = 1.0f / (max.x - min.x);
    float sy = 1.0f / (max.y - min.y);
    
    for (int i = 0; i < count; i++) {
        const vec2& uv1 = uvsrc[i];
        uvdest[i] = vec2((uv1.x - min.x) * sx, (uv1.y - min.y) * sy);
    }
}

//inline static float checkShrinkValue(const vec3& v, const float value, const float min, const float max) {
//	return 0;
//}

void Mesh::shrinkUV(const int uvIndex, const float value, const float min, const float max) {
    if (uvIndex >= this->uvCount) return;
    
    const int triangleCount = this->getTriangleCount();
    
    vec2 uv1, uv2, uv3;
    
    for (int i = 0; i < triangleCount; i++) {
        this->getUV(uvIndex, i, &uv1, &uv2, &uv3);
        
        //		// middle point
        //		const vec2 base = (uv1 + uv2 + uv3) / 3.0f;
        
        // bounding box
        const BBox2D box = BBox2D::fromTriangle(uv1, uv2, uv3);
        const vec2 base = box.getOrigin();
        
        //		uv1 = uv1 + (base - uv1) * value;
        //		uv2 = uv2 + (base - uv2) * value;
        //		uv3 = uv3 + (base - uv3) * value;
        
        uv1 = uv1 + normalize(base - uv1) * value;
        uv2 = uv2 + normalize(base - uv2) * value;
        uv3 = uv3 + normalize(base - uv3) * value;
        
#if defined(DEBUG) || defined(DEBUG_LOCAL)
        if (uv1.x != uv1.x || uv1.y != uv1.y) {
            printf("shrink uv: invalid texcoords: %f, %f\n", uv1.x, uv1.y);
        }
        if (uv2.x != uv2.x || uv2.y != uv2.y) {
            printf("shrink uv: invalid texcoords: %f, %f\n", uv2.x, uv2.y);
        }
        if (uv3.x != uv3.x || uv3.y != uv3.y) {
            printf("shrink uv: invalid texcoords: %f, %f\n", uv3.x, uv3.y);
        }
#endif /* defined(DEBUG) || defined(DEBUG_LOCAL) */
        
        this->setUV(uvIndex, i, uv1, uv2, uv3);
    }
}

void Mesh::offsetUV(const int uvIndex, const vec2& value) {
    if (uvIndex >= this->uvCount) return;
    
    for (uint i = 0; i < this->vertexCount; i++) {
        vec2& uv = this->texcoords[uvIndex * this->vertexCount + i];
        uv += value;
    }
}

void Mesh::tileUV(const int dir, const vec2& ratio) {
    for (int i = 0; i < this->vertexCount; i++) {
        const vec3& v = this->vertices[i];
        vec2& uv = this->texcoords[i];
        
        switch (dir) {
            case 0: uv = vec2(v.x, v.z) * ratio; break;
            case 1: uv = vec2(v.x, v.y) * ratio; break;
        }
    }
}

void Mesh::calcTangentBasis() {
    if (this->uvCount <= 0 || !this->hasTexcoord) {
        return;
    }
    
    if (this->tangents != NULL) {
        delete [] this->tangents;
    }
    
    if (this->bitangents != NULL) {
        delete [] this->bitangents;
    }
    
    const uint triangleCount = this->getTriangleCount();
    
    vec3* tangents = new vec3[this->vertexCount];
    vec3* bitangents = new vec3[this->vertexCount];
    
    int i = 0, j = 0;
    for (ulong k = 0; k < triangleCount; k++) {
        vec3 v1, v2, v3;
        vec2 uv1, uv2, uv3;
        
        this->getVertex(k, &v1, &v2, &v3);
        this->getUV(0, k, &uv1, &uv2, &uv3);
        
        // edges of the triangle : postion delta
        const vec3 deltaPos1 = v2 - v1;
        const vec3 deltaPos2 = v3 - v1;
        
        // UV delta
        const vec2 deltaUV1 = uv2 - uv1;
        const vec2 deltaUV2 = uv3 - uv1;
        
        float r = 1.0f / (deltaUV1.x * deltaUV2.y - deltaUV1.y * deltaUV2.x);
        const vec3 tangent = -((deltaPos1 * deltaUV2.y - deltaPos2 * deltaUV1.y) * r).normalize();
        const vec3 bitangent = -((deltaPos2 * deltaUV1.x - deltaPos1 * deltaUV2.x) * r).normalize();
        
        // set the same tangent and bitnormals for all three vertices of the triangle
        tangents[i++] = tangent; tangents[i++] = tangent; tangents[i++] = tangent;
        bitangents[j++] = bitangent; bitangents[j++] = bitangent; bitangents[j++] = bitangent;
    }
    
    this->tangents = tangents;
    this->bitangents = bitangents;
    
    this->hasTangentSpaceBasis = true;
}

void Mesh::setBounds(const BoundingBox& box) {
    const auto selfbox = this->getBoundingBox();
    Matrix4 m;
    m.translate(box.origin - selfbox.origin);
    m.scale(vec3::one + box.size - selfbox.size);
    this->applyTransform(m);
}

color3* Mesh::createColorBuffer() {
    this->colors = new color3[this->vertexCount];
    this->hasColor = true;
    
    return this->colors;
}

inline static int indexOfAlmostSameEdgeInList(const EdgeList& edges, const Edge& e) {
    for (int i = 0; i < edges.size(); i++) {
        const Edge& ee  = edges[i];
        
        if (Edge::almostSame(ee, e)) {
            return i;
        }
    }
    
    return -1;
}

void Mesh::generateWireframe() {
    if (this->edges != NULL) {
        delete this->edges;
        this->edges = NULL;
        this->edgeCount = 0;
    }
    
    EdgeList edges;
    this->getWireframeEdges(edges);
    
    if (edges.size() > 0) {
        this->edgeCount = (uint)edges.size();
        this->edges = new Edge[this->edgeCount];
        memcpy(this->edges, edges.data(), this->edgeCount * sizeof(Edge));
    }
}

void Mesh::getEdges(EdgeList& edges) {
    for (int t = 0; t < this->getTriangleCount(); t++) {
        vec3 v1, v2, v3;
        this->getVertex(t, &v1, &v2, &v3);
        
        Edge e1(v1, v2), e2(v2, v3), e3(v2, v3);
        edges.push_back(e1);
        edges.push_back(e2);
        edges.push_back(e3);
    }
}

void Mesh::getWireframeEdges(EdgeList& edges) {
    for (int t = 0; t < this->getTriangleCount(); t++) {
        vec3 v1, v2, v3;
        this->getVertex(t, &v1, &v2, &v3);
        
        Edge e1(v1, v2), e2(v2, v3), e3(v1, v3);
        const float e1len = e1.length(), e2len = e2.length(), e3len	= e3.length();
        
        if (e1len > e2len && e1len > e3len) {
            if (indexOfAlmostSameEdgeInList(edges, e2) < 0) {
                edges.push_back(e2);
            }
            if (indexOfAlmostSameEdgeInList(edges, e3) < 0) {
                edges.push_back(e3);
            }
        }
        else if (e2len > e1len && e2len > e3len) {
            if (indexOfAlmostSameEdgeInList(edges, e1) < 0) {
                edges.push_back(e1);
            }
            if (indexOfAlmostSameEdgeInList(edges, e3) < 0) {
                edges.push_back(e3);
            }
        }
        else if (e3len > e1len && e3len > e2len) {
            if (indexOfAlmostSameEdgeInList(edges, e1) < 0) {
                edges.push_back(e1);
            }
            if (indexOfAlmostSameEdgeInList(edges, e2) < 0) {
                edges.push_back(e2);
            }
        }
    }
}

void Mesh::getDistinctEdges(EdgeList& edges) {
    for (int t = 0; t < this->getTriangleCount(); t++) {
        vec3 v1, v2, v3;
        this->getVertex(t, &v1, &v2, &v3);
        
        Edge e1(v1, v2), e2(v2, v3), e3(v1, v3);
        
        if (indexOfAlmostSameEdgeInList(edges, e1) < 0) {
            edges.push_back(e1);
        }
        if (indexOfAlmostSameEdgeInList(edges, e2) < 0) {
            edges.push_back(e2);
        }
        if (indexOfAlmostSameEdgeInList(edges, e3) < 0) {
            edges.push_back(e3);
        }
    }
}

////////////////////// MeshBuffer //////////////////////

void MeshBuffer::appendMesh(const Mesh& mesh) {
    for (int i = 0; i < mesh.vertexCount; i++) {
        this->vertices.push_back(mesh.vertices[i]);
        
        if (mesh.hasNormal) this->normals.push_back(mesh.normals[i]);
        
        if (mesh.hasTexcoord) {
            while (this->texcoords.size() < mesh.uvCount) {
                this->texcoords.push_back(std::vector<vec2>());
            }
            for (int k = 0; k < mesh.uvCount; k++) {
                this->texcoords[k].push_back(mesh.texcoords[k * mesh.vertexCount + i]);
            }
        }
        
        if (mesh.hasColor) this->colors.push_back(mesh.colors[i]);
    }
}

Mesh* MeshBuffer::createMesh() {
    const size_t vertexCount = this->vertices.size();
    
    Mesh* mesh = new Mesh();
    
    mesh->hasNormal = this->normals.size() > 0;
    mesh->hasTexcoord = this->texcoords.size() > 0;
    mesh->hasColor = this->colors.size() > 0;
    
    mesh->init((uint)vertexCount, (uint)this->texcoords.size());
    
    memcpy(mesh->vertices, this->vertices.data(), vertexCount * sizeof(vec3));
    
    if (mesh->hasNormal) {
        memcpy(mesh->normals, this->normals.data(), vertexCount * sizeof(vec3));
    }
    
    if (mesh->hasTexcoord) {
        for (int k = 0; k < this->texcoords.size(); k++) {
            memcpy(mesh->texcoords + k * vertexCount, this->texcoords[k].data(), vertexCount * sizeof(vec2));
        }
    }
    
    if (mesh->hasColor) {
        memcpy(mesh->colors, this->colors.data(), vertexCount * sizeof(color3));
    }
    
    mesh->calcBoundingBox();
    
    return mesh;
}

void MeshBuffer::clear() {
    this->vertices.clear();
    this->normals.clear();
    this->texcoords.clear();
    this->colors.clear();
}

////////////////////// LightmapUVGenerator //////////////////////

LightmapUVGenerator::LightmapUVGenerator(Mesh* mesh) {
    this->mesh = mesh;
}

void LightmapUVGenerator::evaluateAllPolygons() {
    
    const uint triangleCount = this->mesh->getTriangleCount();
    
    vec3 v1, v2, v3;
    vec3 n1, n2, n3;
    
    for (uint i = 0; i < triangleCount; i++) {
        this->mesh->getVertex(i, &v1, &v2, &v3);
        this->mesh->getNormal(i, &n1, &n2, &n3);
        
        const float e1 = length(v2 - v1), e2 = length(v3 - v2), e3 = length(v3 - v1);
        const float s = (e1 + e2 + e3) * 0.5;
        const float as = s * (s - e1) * (s - e2) * (s - e3);
        EvaluatedPolygon et(i, as);
        //		EvaluatedPolygon et(i, length(cross(v2 - v1, v3 - v1)) + length(cross(v1 - v2, v3 - v2)));
        
        et.faceNormal = (n1 + n2 + n3) / 3.0f;
        //et.faceNormal = cross(v2 - v1, v3 - v1).normalize();
        
        vec3 absFaceNormal = abs(et.faceNormal);
        
        if (absFaceNormal.x > absFaceNormal.y && absFaceNormal.x > absFaceNormal.z) {
            et.faceAxis = et.faceNormal.x > 0 ? MESH_FA_X_P : MESH_FA_X_M;
        } else if (absFaceNormal.y > absFaceNormal.x && absFaceNormal.y > absFaceNormal.z) {
            et.faceAxis = et.faceNormal.y > 0 ? MESH_FA_Y_P : MESH_FA_Y_M;
        } else {
            et.faceAxis = et.faceNormal.z > 0 ? MESH_FA_Z_P : MESH_FA_Z_M;
        }
        
        this->scoredTriangles.push_back(et);
    }
    
    std::sort(scoredTriangles.begin(), scoredTriangles.end(), [](const EvaluatedPolygon& p1, const EvaluatedPolygon& p2) -> bool
              {
        return p1.score > p2.score;
    });
}

void LightmapUVGenerator::unwrapAllTriangles() {
    int nextTid;
    
    memset(this->uvBuffer, 0, sizeof(vec2) * this->mesh->vertexCount);
    
    while ((nextTid = this->getNextUnwrapPolygon()) < (int)this->scoredTriangles.size()) {
        this->unwrapPolygon2(nextTid);
    }
    
    this->normalizeUVs();
}

void LightmapUVGenerator::unwrapPolygon(const uint tid) {
    const EvaluatedPolygon& p = scoredTriangles[tid];
    
    vec3 v1, v2, v3;
    this->mesh->getVertex(p.id, &v1, &v2, &v3);
    
    const vec3 edge1 = v2 - v1, edge2 = v3 - v2; //, edge3 = v3 - v1;
    const float e1len = edge1.length(), e2len = edge2.length();//, e3len = edge3.length();
    
    BBox2D box(vec2::zero, ceiling(vec2(e1len, e2len)) - vec2(uvGridPadding, uvGridPadding));
    box = this->findAvailableAreaOfUVMap(box);
    this->unwrappedAreas.push_back(box);
    
    this->mapPolygonTexcoords(tid, box);
    
    this->uvmapUsedArea.expandTo(box);
}

void LightmapUVGenerator::unwrapPolygon2(const uint tid) {
    const EvaluatedPolygon& basep = this->scoredTriangles[tid];
    
    std::vector<uint> facePolygons;
    facePolygons.push_back(tid);
    
    this->findFaceSharedEdgePolygons(tid, facePolygons);
    
    BBox2D box;
    bool bbFirst = true;
    
    vec3 v1, v2, v3;
    vec2 uv1, uv2, uv3;
    
    for (const uint ftid : facePolygons) {
        const EvaluatedPolygon& tp = scoredTriangles[ftid];
        
        this->mesh->getVertex(tp.id, &v1, &v2, &v3);
        
        switch (basep.faceAxis) {
            default:
            case MESH_FA_X_P: case MESH_FA_X_M: uv1 = vec2(v1.y, v1.z); uv2 = vec2(v2.y, v2.z); uv3 = vec2(v3.y, v3.z); break;
            case MESH_FA_Y_P: case MESH_FA_Y_M: uv1 = vec2(v1.x, v1.z); uv2 = vec2(v2.x, v2.z); uv3 = vec2(v3.x, v3.z); break;
            case MESH_FA_Z_P: case MESH_FA_Z_M: uv1 = vec2(v1.x, v1.y); uv2 = vec2(v2.x, v2.y); uv3 = vec2(v3.x, v3.y); break;
        }
        
        this->mesh->setUV(this->uvIndex, tp.id, uv1, uv2, uv3);
        
        if (bbFirst) {
            box.initAt(uv1);
            bbFirst = false;
        } else {
            box.expandTo(uv1);
        }
        
        box.expandTo(uv2);
        box.expandTo(uv3);
    }
    
#if defined(DEBUG_LOCAL)
    if (box.getWidth() < 0.0000001f || box.getHeight() < 0.0000001f) {
        printf("extreme small texcoords bounds\n");
    }
#endif /* defined(DEBUG_LOCAL) */
    
    //	for (const uint ftid : facePolygons) {
    //		const EvaluatedPolygon& tp = scoredTriangles[ftid];
    //
    //		this->mesh->getUV(this->uvIndex, tp.id, &uv1, &uv2, &uv3);
    //
    //		uv1 -= box.min;
    //		uv2 -= box.min;
    //		uv3 -= box.min;
    //
    //		this->mesh->setUV(this->uvIndex, tp.id, uv1, uv2, uv3);
    //	}
    
    //	const vec2 boxSize = box.getSize();
    //	box.min = vec2::zero;
    //	box.max = boxSize;
    //	box.inflate(vec2(0.01, 0.01));
    
    BBox2D newBox = box;
    //	if (newBox.getWidth() < 0.1) newBox.setWidth(0.1);
    //	if (newBox.getHeight() < 0.1) newBox.setHeight(0.1);
    
    newBox = this->findAvailableAreaOfUVMap2(newBox);
    this->mapFaceTexcoords(facePolygons, box, newBox);
    
    BBox2D boxLarger = newBox;
    boxLarger.inflate(uvGridPadding);
    this->unwrappedAreas.push_back(boxLarger);
    this->uvmapUsedArea.expandTo(boxLarger);
}

void LightmapUVGenerator::mapFaceTexcoords(const std::vector<uint>& facePolygonIds, const BBox2D& box, const BBox2D& newBox) {
    const vec2 offset = newBox.min - box.min;
    
    vec2 scale = newBox.getSize() / box.getSize();
    if (scale.x < 1) scale.x = 1;
    if (scale.y < 1) scale.y = 1;
    
    vec2 uv1, uv2, uv3;
    
    for (const uint fptid : facePolygonIds) {
        EvaluatedPolygon& fp = this->scoredTriangles[fptid];
        fp.used = true;
        
        this->mesh->getUV(this->uvIndex, fp.id, &uv1, &uv2, &uv3);
        
        uv1 = uv1 + (((uv1 - box.min) * scale - (uv1 - box.min))) + offset;
        uv2 = uv2 + (((uv2 - box.min) * scale - (uv2 - box.min))) + offset;
        uv3 = uv3 + (((uv3 - box.min) * scale - (uv3 - box.min))) + offset;
        
        this->mesh->setUV(this->uvIndex, fp.id, uv1, uv2, uv3);
    }
}

uint LightmapUVGenerator::getNextUnwrapPolygon() {
    uint nextTid = this->currentTriangleId;
    
    while (nextTid < this->scoredTriangles.size()) {
        nextTid = this->currentTriangleId;
        
        this->currentTriangleId++;
        
        const EvaluatedPolygon& p = scoredTriangles[nextTid];
        if (!p.used) {
            break;
        }
    }
    
    return nextTid;
}

void LightmapUVGenerator::mapPolygonTexcoords(const uint tid, const BBox2D& box) {
    EvaluatedPolygon& p = scoredTriangles[tid];
    p.used = true;
    
    vec3 v1, v2, v3;
    this->mesh->getVertex(p.id, &v1, &v2, &v3);
    
    const float a1 = dot(v2 - v1, v3 - v1);
    const float a2 = dot(v1 - v2, v3 - v2);
    const float a3 = dot(v1 - v3, v2 - v3);
    
    vec2& uv1 = this->uvBuffer[p.id * 3 + 0];
    vec2& uv2 = this->uvBuffer[p.id * 3 + 1];
    vec2& uv3 = this->uvBuffer[p.id * 3 + 2];
    
    if (a1 < a2 && a1 < a3) {
        uv1 = box.min;
        uv2 = vec2(box.min.x, box.max.y);
        uv3 = vec2(box.max.x, box.min.y);
        
        mapOppositePolygonTexcoords(tid, box, v3, v2);
    }
    else if (a3 < a1 && a3 < a2) {
        uv1 = vec2(box.min.x, box.max.y);
        uv2 = vec2(box.max.x, box.min.y);
        uv3 = box.min;
        
        mapOppositePolygonTexcoords(tid, box, v2, v1);
    }
    else {
        uv1 = vec2(box.max.x, box.min.y);
        uv2 = box.min;
        uv3 = vec2(box.min.x, box.max.y);
        
        mapOppositePolygonTexcoords(tid, box, v3, v1);
    }
}

void LightmapUVGenerator::findFaceSharedEdgePolygons(const uint refpid, std::vector<uint> &faceids) {
    const EvaluatedPolygon& refp = this->scoredTriangles[refpid];
    
    vec3 refv1, refv2, refv3;
    this->mesh->getVertex(refp.id, &refv1, &refv2, &refv3);
    
    vec3 fpv1, fpv2, fpv3;
    
    for (int i = this->currentTriangleId; i < (int)this->scoredTriangles.size(); i++) {
        EvaluatedPolygon& fp = this->scoredTriangles[i];
        if (fp.used) continue;
        
        this->mesh->getVertex(fp.id, &fpv1, &fpv2, &fpv3);
        
        if (fp.faceAxis == refp.faceAxis
            && (Edge::almostSame(refv1, refv2, fpv1, fpv2)
                || Edge::almostSame(refv2, refv3, fpv2, fpv3)
                || Edge::almostSame(refv1, refv3, fpv1, fpv3)
                || Edge::almostSame(refv1, refv2, fpv2, fpv3)
                || Edge::almostSame(refv2, refv3, fpv1, fpv3)
                || Edge::almostSame(refv1, refv3, fpv1, fpv2)
                || Edge::almostSame(refv1, refv2, fpv1, fpv3)
                || Edge::almostSame(refv2, refv3, fpv1, fpv2)
                || Edge::almostSame(refv1, refv3, fpv2, fpv3))) {
            
            // avoid duplication
            bool found = false;
            for (const uint ffid : faceids) {
                if (i == ffid) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                faceids.push_back(i);
                
                this->findFaceSharedEdgePolygons(i, faceids);
            }
        }
    }
}

void LightmapUVGenerator::mapOppositePolygonTexcoords(const uint startTid, const BBox2D& box, const vec3& ev1, const vec3& ev2) {
    vec3 v1, v2, v3;
    
    for (uint i = startTid + 1; i < this->scoredTriangles.size(); i++) {
        EvaluatedPolygon& p = this->scoredTriangles[i];
        
        if (p.used) continue;
        
        this->mesh->getVertex(p.id, &v1, &v2, &v3);
        
        vec2& uv1 = this->uvBuffer[p.id * 3 + 0];
        vec2& uv2 = this->uvBuffer[p.id * 3 + 1];
        vec2& uv3 = this->uvBuffer[p.id * 3 + 2];
        
        if (Edge::almostSame(v3, v2, ev1, ev2)) {
            uv1 = box.max;
            uv2 = vec2(box.max.x, box.min.y);
            uv3 = vec2(box.min.x, box.max.y);
            
            p.used = true;
            break;
        }
        else if (Edge::almostSame(v2, v1, ev1, ev2)) {
            uv1 = vec2(box.max.x, box.min.y);
            uv2 = vec2(box.min.x, box.max.y);
            uv3 = box.max;
            
            p.used = true;
            break;
        }
        else if (Edge::almostSame(v3, v1, ev1, ev2)) {
            uv1 = vec2(box.min.x, box.max.y);
            uv2 = box.max;
            uv3 = vec2(box.max.x, box.min.y);
            
            p.used = true;
            break;
        }
    }
}

BBox2D LightmapUVGenerator::findAvailableAreaOfUVMap(const BBox2D& boxref) {
    BBox2D box = boxref;
    boxTryCount = 0;
    
    const vec2 boxSize = box.getSize();
    
    for (float y = uvGridPadding; y <= this->uvmapUsedArea.max.y + uvGridSize; y += uvGridSize) {
        for (float x = this->uvmapUsedArea.max.x + uvGridSize + uvGridPadding; x >= 0; x -= uvGridSize) {
            vec2 pos = vec2(x, y);
            
            box.min = pos;
            box.max = pos + boxSize;
            
            if (this->isAreaUsed(box)) {
                break;
            }
            
            vec2 newSize = this->uvmapUsedArea.getSize();
            
            if (newSize.x < box.max.x) newSize.x = box.max.x;
            if (newSize.y < box.max.y) newSize.y = box.max.y;
            
            boxTries[boxTryCount++] = LayoutBox(pos, newSize.area() - this->uvmapUsedArea.getSize().area());
            
            if (boxTryCount >= LAYOUT_RETRY_LIMIT) break;
        }
        
        if (boxTryCount >= LAYOUT_RETRY_LIMIT) break;
    }
    
    int bestid = 0;
    float minIncreasedArea = boxTries[0].increasedArea;
    vec2 minPos;
    
    for (int i = 1; i < boxTryCount; i++) {
        const LayoutBox& lb = boxTries[i];
        
        if (lb.increasedArea < minIncreasedArea) {
            minIncreasedArea = lb.increasedArea;
            bestid = i;
            minPos = lb.pos;
        }
        else if (fabsf(minIncreasedArea - lb.increasedArea) < 0.00001f) {
            if (lb.pos.x < minPos.x) {
                minPos = lb.pos;
                bestid = i;
            } else if (lb.pos.y < minPos.y) {
                minPos = lb.pos;
                bestid = i;
            }
        }
    }
    
    const vec2& pos = boxTries[bestid].pos;
    
    box.min = pos;
    box.max = pos + boxSize;
    
    return box;
}

BBox2D LightmapUVGenerator::findAvailableAreaOfUVMap2(const BBox2D& boxref) {
    BBox2D box = boxref;
    
    const vec2 boxSize = box.getSize();
    
    float bestAR = this->uvmapUsedArea.getSize().aspectRate();
    vec2 bestpos;
    
    bool first = true;
    bool found = false;
    
    boxTryCount = 0;
    for (float k = uvGridPadding; !found; k += uvGridPadding) {
        for (float y = uvGridPadding; y < k; y += uvGridPadding) {
            vec2 pos = vec2(k, y);
            
            if (this->checkCandidateBoxArea(pos, box, boxSize, bestAR, bestpos, first)) {
                found = true;
                break;
            }
            
            boxTryCount++;
        }
        
        for (float x = uvGridPadding; x < k; x += uvGridPadding) {
            vec2 pos = vec2(x, k);
            
            if (this->checkCandidateBoxArea(pos, box, boxSize, bestAR, bestpos, first)) {
                found = true;
                break;
            }
            
            boxTryCount++;
        }
    }
    
    box.min = bestpos;
    box.max = bestpos + boxSize;
    
    return box;
}

bool LightmapUVGenerator::checkCandidateBoxArea(vec2& pos, BBox2D& box, const vec2& boxSize, float& bestAR, vec2& bestpos, bool& first) {
    box.min = pos;
    box.max = pos + boxSize;
    
    if (!this->isAreaUsed(box)) {
        vec2 newSize = this->uvmapUsedArea.getSize();
        
        if (newSize.x < box.max.x) newSize.x = box.max.x;
        if (newSize.y < box.max.y) newSize.y = box.max.y;
        
        const float newAR = newSize.aspectRate();
        
        if (first) {
            bestpos = pos;
            first = false;
            return true;
        } else if (fabsf(newAR - 1.0f) < fabsf(bestAR - 1.0f)) {
            bestAR = newAR;
            bestpos = pos;
            return true;
        }
    }
    
    return false;
}

bool LightmapUVGenerator::isAreaUsed(const BBox2D& box) const {
    for (const BBox2D& abox : unwrappedAreas) {
        if (abox.intersects(box)) {
            return true;
        }
    }
    
    return false;
}

void LightmapUVGenerator::normalizeUVs() {
    //const float max = fmaxf(uvmapMaxSize.x, uvmapMaxSize.y);
    //const vec2 invS = vec2(1.0f / (max + uvGridPadding), 1.0f / (max + uvGridPadding));
    const vec2 usedAreaSize = this->uvmapUsedArea.getSize();
    const vec2 invS = vec2(1.0f / (usedAreaSize.x + uvGridPadding), 1.0f / (usedAreaSize.y + uvGridPadding));
    
    for (uint i = 0; i < this->mesh->vertexCount; i++) {
        uvBuffer[i] *= invS;
    }
    
    uvmapUsedArea *= invS;
}

bool LightmapUVGenerator::generate(const int uvIndex) {
    this->uvIndex = uvIndex;
    
    if (uvIndex >= mesh->uvCount) {
        this->uvBuffer = mesh->appendUVBuffer(uvIndex + 1 - mesh->uvCount);
    } else {
        this->uvBuffer = mesh->getUVBuffer(this->uvIndex);
    }
    
    this->evaluateAllPolygons();
    this->unwrapAllTriangles();
    
    return true;
}

}
