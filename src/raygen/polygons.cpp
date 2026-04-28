///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include <utility>

#include "polygons.h"

namespace raygen {

PlaneMesh::PlaneMesh(const int w, const int h) {
    const float halfw = w * 0.5;
    const float halfh = h * 0.5;

    this->create(vec3(-halfw, 0, -halfh), vec3(halfw, 0, halfh));
}

PlaneMesh::PlaneMesh(const vec2& from, const vec2& to)
: PlaneMesh(vec3(from.x, 0, from.y), vec3(to.x, 0, to.y)) {
}

PlaneMesh::PlaneMesh(const vec3& from, const vec3& to)
: PlaneMesh() {
    this->create(from, to);
}

void PlaneMesh::create(const vec3& from, const vec3& to) {
    this->hasNormal = true;
    this->hasTexcoord = true;
    
    this->init(6);

    // Fix Y to 0 for a true plane in the XZ axis
    this->vertices[0] = vec3(from.x, 0.0f, from.z);
    this->vertices[1] = vec3(from.x, 0.0f, to.z);
    this->vertices[2] = vec3(to.x, 0.0f, from.z);

    this->vertices[3] = vec3(from.x, 0.0f, to.z);
    this->vertices[4] = vec3(to.x, 0.0f, to.z);
    this->vertices[5] = vec3(to.x, 0.0f, from.z);

    // Set normals explicitly as (0, 1, 0)
    for (int i = 0; i < 6; i++) {
        this->normals[i] = vec3(0.0f, 1.0f, 0.0f);
    }
    
    this->texcoords[0] = vec2(0, 0);
    this->texcoords[1] = vec2(0, 1);
    this->texcoords[2] = vec2(1, 0);
    
    this->texcoords[3] = vec2(0, 1);
    this->texcoords[4] = vec2(1, 1);
    this->texcoords[5] = vec2(1, 0);
    
    this->calcTangentBasis();
    this->calcBoundingBox();
}

CubeMesh::CubeMesh() {
    this->hasNormal = true;
    this->hasTexcoord = true;
    this->hasTangentSpaceBasis = false; // TODO

    this->init(36);

    // Define cube vertices and normals
    const vec3 positions[8] = {
        vec3(-0.5f, -0.5f, -0.5f), vec3(0.5f, -0.5f, -0.5f),
        vec3(0.5f,  0.5f, -0.5f), vec3(-0.5f,  0.5f, -0.5f),
        vec3(-0.5f, -0.5f,  0.5f), vec3(0.5f, -0.5f,  0.5f),
        vec3(0.5f,  0.5f,  0.5f), vec3(-0.5f,  0.5f,  0.5f),
    };

    const int faces[6][4] = {
        { 0, 1, 2, 3 }, // back
        { 4, 5, 6, 7 }, // front
        { 0, 4, 7, 3 }, // left
        { 1, 5, 6, 2 }, // right
        { 3, 2, 6, 7 }, // top
        { 0, 1, 5, 4 }, // bottom
    };

    const vec3 normals[6] = {
        vec3(0, 0, -1), vec3(0, 0, 1), vec3(-1, 0, 0),
        vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, -1, 0)
    };

    const vec2 texcoords[4] = {
        vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1)
    };

    int idx = 0;
    for (int i = 0; i < 6; ++i) {
        int v0 = faces[i][0];
        int v1 = faces[i][1];
        int v2 = faces[i][2];
        int v3 = faces[i][3];

        // First triangle
        this->vertices[idx] = positions[v0];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[0];

        this->vertices[idx] = positions[v1];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[1];

        this->vertices[idx] = positions[v2];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[2];

        // Second triangle
        this->vertices[idx] = positions[v0];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[0];

        this->vertices[idx] = positions[v2];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[2];

        this->vertices[idx] = positions[v3];
        this->normals[idx] = normals[i];
        this->texcoords[idx++] = texcoords[3];
    }

    this->calcTangentBasis();
    this->calcBoundingBox();
}

SphereMesh::SphereMesh(float radius, int stacks, int slices) {
    
    this->vertexCount = (stacks + 1) * (slices + 1);
    this->indexCount = stacks * slices * 6;

    this->vertices = new vec3[this->vertexCount];
    this->normals = new vec3[this->vertexCount];
    this->texcoords = new vec2[this->vertexCount];
    this->indexes = new vertex_index_t[this->indexCount];

    int vIndex = 0;
    for (int i = 0; i <= stacks; ++i) {
        float v = (float)i / stacks;
        float phi = v * M_PI;

        for (int j = 0; j <= slices; ++j) {
            float u = (float)j / slices;
            float theta = u * 2.0f * M_PI;

            float x = sinf(phi) * cosf(theta);
            float y = cosf(phi);
            float z = sinf(phi) * sinf(theta);

            this->vertices[vIndex] = vec3(x, y, z) * radius;
            this->normals[vIndex] = vec3(x, y, z);
            this->texcoords[vIndex] = vec2(u, v);
            ++vIndex;
        }
    }

    int idx = 0;
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            int cur = i * (slices + 1) + j;
            int next = (i + 1) * (slices + 1) + j;

            this->indexes[idx++] = cur;
            this->indexes[idx++] = next;
            this->indexes[idx++] = cur + 1;

            this->indexes[idx++] = cur + 1;
            this->indexes[idx++] = next;
            this->indexes[idx++] = next + 1;
        }
    }

    this->hasNormal = true;
    this->hasTexcoord = true;
}

ConeMesh::ConeMesh(float radiusStart, float radiusEnd, int slices) {
    this->hasNormal = true;
    this->hasTexcoord = true;
    this->hasTangentSpaceBasis = false;

    if (slices < 3) slices = 3;

    // radiusStart sits at z = +0.5 (object-local "forward"), radiusEnd at
    // z = -0.5. Swap into the internal (small-z, large-z) build order so
    // existing cap winding / slant-normal math stays valid.
    std::swap(radiusStart, radiusEnd);

    const float zStart = -0.5f;
    const float zEnd   =  0.5f;
    const float height = zEnd - zStart;

    // Slant-side normal lives in the (radial, z) plane. radial component
    // is +1 (outward), z component is the slope dr/dz inverted in sign so
    // it points outward along the slant. Normalize once; rotate per-slice.
    const float slope     = (radiusStart - radiusEnd) / height;
    const float invLen    = 1.0f / sqrtf(1.0f + slope * slope);
    const float radialN   = invLen;
    const float axialN    = slope * invLen;

    const bool capStart = radiusStart > 0.0f;
    const bool capEnd   = radiusEnd   > 0.0f;

    const int sideTri = slices * 2;
    const int capTri  = (capStart ? slices : 0) + (capEnd ? slices : 0);
    const int vCount  = (sideTri + capTri) * 3;

    this->init(vCount);

    int idx = 0;

    // Side wall: two triangles per slice spanning start ring to end ring.
    for (int i = 0; i < slices; ++i) {
        const float t0 = (float)i       / slices;
        const float t1 = (float)(i + 1) / slices;
        const float a0 = t0 * 2.0f * M_PI;
        const float a1 = t1 * 2.0f * M_PI;
        const float c0 = cosf(a0), s0 = sinf(a0);
        const float c1 = cosf(a1), s1 = sinf(a1);

        const vec3 vS0(c0 * radiusStart, s0 * radiusStart, zStart);
        const vec3 vS1(c1 * radiusStart, s1 * radiusStart, zStart);
        const vec3 vE0(c0 * radiusEnd,   s0 * radiusEnd,   zEnd);
        const vec3 vE1(c1 * radiusEnd,   s1 * radiusEnd,   zEnd);

        const vec3 n0(c0 * radialN, s0 * radialN, axialN);
        const vec3 n1(c1 * radialN, s1 * radialN, axialN);

        // Triangle 1: vS0 -> vE0 -> vE1
        this->vertices[idx]  = vS0; this->normals[idx] = n0;
        this->texcoords[idx] = vec2(t0, 0); ++idx;
        this->vertices[idx]  = vE0; this->normals[idx] = n0;
        this->texcoords[idx] = vec2(t0, 1); ++idx;
        this->vertices[idx]  = vE1; this->normals[idx] = n1;
        this->texcoords[idx] = vec2(t1, 1); ++idx;

        // Triangle 2: vS0 -> vE1 -> vS1
        this->vertices[idx]  = vS0; this->normals[idx] = n0;
        this->texcoords[idx] = vec2(t0, 0); ++idx;
        this->vertices[idx]  = vE1; this->normals[idx] = n1;
        this->texcoords[idx] = vec2(t1, 1); ++idx;
        this->vertices[idx]  = vS1; this->normals[idx] = n1;
        this->texcoords[idx] = vec2(t1, 0); ++idx;
    }

    // Start cap (z = -0.5, normal -Z). Wound CW from outside.
    if (capStart) {
        const vec3 n(0, 0, -1);
        for (int i = 0; i < slices; ++i) {
            const float a0 = (float)i       / slices * 2.0f * M_PI;
            const float a1 = (float)(i + 1) / slices * 2.0f * M_PI;
            const float c0 = cosf(a0), s0 = sinf(a0);
            const float c1 = cosf(a1), s1 = sinf(a1);

            this->vertices[idx]  = vec3(0, 0, zStart); this->normals[idx] = n;
            this->texcoords[idx] = vec2(0.5f, 0.5f); ++idx;
            this->vertices[idx]  = vec3(c1 * radiusStart, s1 * radiusStart, zStart);
            this->normals[idx]   = n;
            this->texcoords[idx] = vec2(0.5f + 0.5f * c1, 0.5f + 0.5f * s1); ++idx;
            this->vertices[idx]  = vec3(c0 * radiusStart, s0 * radiusStart, zStart);
            this->normals[idx]   = n;
            this->texcoords[idx] = vec2(0.5f + 0.5f * c0, 0.5f + 0.5f * s0); ++idx;
        }
    }

    // End cap (z = +0.5, normal +Z).
    if (capEnd) {
        const vec3 n(0, 0, 1);
        for (int i = 0; i < slices; ++i) {
            const float a0 = (float)i       / slices * 2.0f * M_PI;
            const float a1 = (float)(i + 1) / slices * 2.0f * M_PI;
            const float c0 = cosf(a0), s0 = sinf(a0);
            const float c1 = cosf(a1), s1 = sinf(a1);

            this->vertices[idx]  = vec3(0, 0, zEnd); this->normals[idx] = n;
            this->texcoords[idx] = vec2(0.5f, 0.5f); ++idx;
            this->vertices[idx]  = vec3(c0 * radiusEnd, s0 * radiusEnd, zEnd);
            this->normals[idx]   = n;
            this->texcoords[idx] = vec2(0.5f + 0.5f * c0, 0.5f + 0.5f * s0); ++idx;
            this->vertices[idx]  = vec3(c1 * radiusEnd, s1 * radiusEnd, zEnd);
            this->normals[idx]   = n;
            this->texcoords[idx] = vec2(0.5f + 0.5f * c1, 0.5f + 0.5f * s1); ++idx;
        }
    }

    this->calcTangentBasis();
    this->calcBoundingBox();
}

}
