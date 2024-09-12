///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef mesh_h
#define mesh_h

#include <stdio.h>
#include <vector>

#include "ucm/types.h"
#include "ugm/types2d.h"
#include "ugm/color.h"
#include "ugm/matrix.h"
#include "ugm/functions.h"
#include "ugm/image.h"
#include "texture.h"
#include "cubetex.h"

//#define ENABLE_UINT_INDEX

#if defined(ENABLE_UINT_INDEX)
typedef uint vertex_index_t;
#else
typedef ushort vertex_index_t;
#endif /* ENABLE_UINT_INDEX */

namespace raygen {

struct GrabBoundary {
    float left;
    float right;
    float top;
    float bottom;
    float front;
    float back;
    
    GrabBoundary() : left(0), right(0), top(0), bottom(0), front(0), back(0) { }
};

struct Edge {
    vec3 v1;
    vec3 v2;
    
    Edge() { }
    Edge(const vec3& v1, const vec3& v2) : v1(v1), v2(v2) { }
    
    inline float length() {
        return (v1 - v2).length();
    }
    
    static bool almostSame(const Edge& e1, const Edge& e2) {
        return almostSame(e1.v1, e1.v2, e2.v1, e2.v2);
    }
    
    static bool almostSame(const vec2& e1v1, const vec2& e1v2, const vec2& e2v1, const vec2& e2v2) {
        return (extremelyClose(e1v1, e2v1) && extremelyClose(e1v2, e2v2))
        || (extremelyClose(e1v1, e2v2) && extremelyClose(e1v2, e2v1));
    }
    
    static bool almostSame(const vec3& e1v1, const vec3& e1v2, const vec3& e2v1, const vec3& e2v2) {
        return (extremelyClose(e1v1, e2v1) && extremelyClose(e1v2, e2v2))
        || (extremelyClose(e1v1, e2v2) && extremelyClose(e1v2, e2v1));
    }
    
    static bool almostEqual(const vec3& e1v1, const vec3& e1v2, const vec3& e2v1, const vec3& e2v2) {
        return (extremelyClose(e1v1, e2v1) && extremelyClose(e1v2, e2v2));
    }
    
    bool operator==(const Edge& e) const {
        return this->v1 == e.v1 && this->v2 == e.v2;
    }
};

typedef std::vector<Edge> EdgeList;

class Mesh {
private:
    
public:
    uint vertexCount = 0;
    uint uvCount = 0;
    uint indexCount = 0;
    uint edgeCount = 0;
    
    bool hasNormal = false;
    bool hasTexcoord = false;
    bool hasTangentSpaceBasis = false;
    bool hasBoundingBox = false;
    bool hasColor = false;
    bool hasLightmap = false;
    bool hasRefmap = false;
    bool hasGrabBoundary = false;
    
    vec3* vertices = NULL;
    vec3* normals = NULL;
    vec2* texcoords = NULL;
    vec3* tangents = NULL;
    vec3* bitangents = NULL;
    vertex_index_t* indexes = NULL;
    color3* colors = NULL;
    Edge* edges = NULL;
    
    BoundingBox bbox;
    GrabBoundary grabBoundary;
    //	BBox2D* uvLayout = NULL;
    
    struct {
        void* rendererData = NULL;
        uint trunkUid = 0;
        uint lightmapTrunkUid = 0;
        Texture* lightmap = NULL;
        uint refmapTrunkUid = 0;
        CubeTexture* refmap = NULL;
        //		MeshLightmapTypes lightmapType = MLT_UNKNOWN;
    };
    
    void setLightmap(Texture* lightmap) {
        this->lightmap = lightmap;
        this->hasLightmap = lightmap != NULL;
    }
    
    void setRefmap(CubeTexture* cubetex) {
        this->refmap = cubetex;
        this->hasRefmap = cubetex != NULL;
    }
    
    ~Mesh();
    
    void init(const uint vertexCount, const uint uvCount = 1, const uint indexCount = 0);
    
    inline uint getTriangleCount() const {
        return this->indexCount > 0 ? (this->indexCount / 3) : (this->vertexCount / 3);
    }
    
    void getIndexes(const ulong triangleIndex, vertex_index_t& i1, vertex_index_t& i2, vertex_index_t& i3) const;
    void getVertex(const ulong triangleIndex, vec3* v1, vec3* v2, vec3* v3) const;
    void setVertex(const ulong triangleIndex, const vec3& v1, const vec3& v2, const vec3& v3);
    void getNormal(const ulong triangleIndex, vec3* n1, vec3* n2, vec3* n3) const;
    void setNormal(const ulong traingleIndex, const vec3& n1, const vec3& n2, const vec3& n3);
    void getColor(const ulong triangleIndex, color3* c1, color3* c2, color3* c3) const;
    void setColor(const ulong triangleIndex, const color3& c1, const color3& c2, const color3& c3);
    void getUV(const uint uvIndex, const ulong triangleNumber, vec2* uv1, vec2* uv2, vec2* uv3) const;
    void setUV(const uint uvIndex, const ulong triangleNumber, const vec2& uv1, const vec2& uv2, const vec2& uv3);
    void getTriangleNUV(const ulong triangleIndex, const uint uvIndex, TriangleNUV* tnuv);
    void setTriangleNUV(const ulong triangleIndex, const uint uvIndex, const TriangleNUV& tnuv);
    void getTriangleNUV2TBC(const ulong triangleIndex, TriangleNUV2TBC* tnuv);
    void setTriangleNUV2TBC(const ulong triangleIndex, const TriangleNUV2TBC& tnuv);
    
    // utility
    ulong calcMemorySize() const;
    void resizeVertexCount(int newVertexCount);
    Mesh* clone() const;
    static void copy(const Mesh& m1, Mesh& m2);
    
    void inversePolygonVertexOrder(bool applyTexcoord = true);
    void calcNormals();
    void flipNormals();
    void extractIndex();
    void composeIndex();
    
    void applyTransform(const Matrix4& m);
    void offset(const vec3& v);
    vec3 alignToOrigin();
    
    BoundingBox calcBoundingBox();
    BoundingBox getBoundingBox();
    void calcTangentBasis();
    void setBounds(const BoundingBox& box);
    
    // deform utility
    void subdivideTriangles(const float minLength);
    static bool subdivideTriangle(const std::vector<TriangleNUV2TBC>& triangles, const int triangleIndex, const float minLength, TriangleNUV2TBC ts[2]);
    void grabVertices(const GrabBoundary& offset);
    void grabResize(const GrabBoundary& newSize);
    
    // uv utility
    void inverseTexcoordV();
    static void normalizeUV(vec2* uvdest, const vec2* uvsrc, const int count);
    void shrinkUV(const int uvIndex, const float value = 0.005f, const float min = 0.01f, const float max = 0.1f);
    void offsetUV(const int uvIndex, const vec2& value);
    void tileUV(const int dir, const vec2& ratio);
    
    // uv buffer
    vec2* appendUVBuffer(const uint newCount = 1);
    vec2* getUVBuffer(const int index);
    const vec2* getUVBuffer(const int index) const;
    bool hasUVBuffer(const int index) const;
    void generateUV2FromUV1();
    
    // color
    color3* createColorBuffer();
    
    // edge & wireframe
    void generateWireframe();
    void getEdges(EdgeList& edges);
    void getWireframeEdges(EdgeList& edges);
    void getDistinctEdges(EdgeList& edges);
};

////////////////////// MeshBuffer //////////////////////

class MeshBuffer {
    std::vector<vec3> vertices;
    std::vector<vec3> normals;
    std::vector<std::vector<vec2>> texcoords;
    std::vector<color3> colors;
    
public:
    void appendMesh(const Mesh& mesh);
    Mesh* createMesh();
    
    void clear();
    
    const int getVertexCount() const { return (int)this->vertices.size(); }
};

///////////////////// LightmapUVGenerator /////////////////////

#define LAYOUT_RETRY_LIMIT 1

class LightmapUVGenerator {
private:
    uint uvIndex = 0;
    Mesh* mesh = NULL;
    
    enum FaceAxis {
        MESH_FA_X_P, MESH_FA_Y_P, MESH_FA_Z_P,
        MESH_FA_X_M, MESH_FA_Y_M, MESH_FA_Z_M,
    };
    
    struct EvaluatedPolygon {
        uint id = 0;
        float score = 0;
        vec3 faceNormal;
        FaceAxis faceAxis = MESH_FA_X_P;
        bool used = false;
        
        EvaluatedPolygon(const uint id, const float score) : id(id), score(score) { }
    };
    
    struct LayoutBox {
        vec2 pos;
        float increasedArea = 0;
        
        LayoutBox() { }
        LayoutBox(const vec2 pos, const float increasedArea) : pos(pos), increasedArea(increasedArea) { }
    };
    
    static constexpr float uvGridSize = 0.05f;
    static constexpr float uvGridPadding = 0.01f;
    
    vec2* uvBuffer = NULL;
    std::vector<EvaluatedPolygon> scoredTriangles;
    std::vector<BBox2D> unwrappedAreas;
    BBox2D uvmapUsedArea;
    uint currentTriangleId = 0;
    LayoutBox boxTries[LAYOUT_RETRY_LIMIT];
    int boxTryCount = 0;
    
    void evaluateAllPolygons();
    void unwrapAllTriangles();
    void unwrapPolygon(const uint tid);
    void unwrapPolygon2(const uint tid);
    uint getNextUnwrapPolygon();
    void findFaceSharedEdgePolygons(const uint tid, std::vector<uint>& tids);
    void mapPolygonTexcoords(const uint tid, const BBox2D& box);
    void mapOppositePolygonTexcoords(const uint startTid, const BBox2D& box, const vec3& ev1, const vec3& ev2);
    void mapFaceTexcoords(const std::vector<uint>& facePolygons, const BBox2D& box, const BBox2D& newBox);
    BBox2D findAvailableAreaOfUVMap(const BBox2D& box);
    BBox2D findAvailableAreaOfUVMap2(const BBox2D& box);
    bool checkCandidateBoxArea(vec2& pos, BBox2D& box, const vec2& boxSize, float& bestAR, vec2& bestpos, bool& first);
    bool isAreaUsed(const BBox2D& box) const;
    void normalizeUVs();
    
public:
    LightmapUVGenerator(Mesh* mesh);
    bool generate(const int uvNumber);
};

}

#endif /* mesh_h */
