///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

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
    
}
