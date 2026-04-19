///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "bvh.h"
#include <algorithm>
#include <cmath>
#include <cfloat>

namespace raygen {

using ugm::vec3;
using ugm::Ray;
using ugm::BoundingBox;

namespace {

constexpr int BVH_BINS = 16;
constexpr uint16_t BVH_LEAF_SIZE = 4;  // stop splitting at this many prims
constexpr float BVH_TRAVERSAL_COST = 1.0f;
constexpr float BVH_INTERSECT_COST = 1.5f;

struct Bin {
    BoundingBox bbox;
    int count = 0;
    Bin() {
        bbox.min = vec3( FLT_MAX,  FLT_MAX,  FLT_MAX);
        bbox.max = vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    }
};

inline void expandBBox(BoundingBox& dst, const BoundingBox& src) {
    dst.min.x = fminf(dst.min.x, src.min.x);
    dst.min.y = fminf(dst.min.y, src.min.y);
    dst.min.z = fminf(dst.min.z, src.min.z);
    dst.max.x = fmaxf(dst.max.x, src.max.x);
    dst.max.y = fmaxf(dst.max.y, src.max.y);
    dst.max.z = fmaxf(dst.max.z, src.max.z);
}

inline void expandBBox(BoundingBox& dst, const vec3& p) {
    dst.min.x = fminf(dst.min.x, p.x);
    dst.min.y = fminf(dst.min.y, p.y);
    dst.min.z = fminf(dst.min.z, p.z);
    dst.max.x = fmaxf(dst.max.x, p.x);
    dst.max.y = fmaxf(dst.max.y, p.y);
    dst.max.z = fmaxf(dst.max.z, p.z);
}

inline float surfaceArea(const BoundingBox& b) {
    const float dx = b.max.x - b.min.x;
    const float dy = b.max.y - b.min.y;
    const float dz = b.max.z - b.min.z;
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

inline BoundingBox emptyBBox() {
    BoundingBox b;
    b.min = vec3( FLT_MAX,  FLT_MAX,  FLT_MAX);
    b.max = vec3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    return b;
}

}  // namespace

void TriangleBVH::reset() {
    nodes.clear();
    prims.clear();
    centroids.clear();
}

void TriangleBVH::build(std::vector<const RenderMeshTriangle*>& inprims) {
    reset();
    if (inprims.empty()) return;

    prims = inprims;
    centroids.resize(prims.size());
    for (size_t i = 0; i < prims.size(); i++) {
        const auto& b = prims[i]->bbox;
        centroids[i] = (b.min + b.max) * 0.5f;
    }

    nodes.reserve(prims.size() * 2);
    nodes.emplace_back();
    buildRecursive(0, 0, (uint32_t)prims.size());

    // Copy the reordered permutation back so the caller sees the same order
    // the BVH uses (matches existing KDTree behaviour which took ownership).
    inprims = prims;

    // Centroids only needed during build.
    centroids.clear();
    centroids.shrink_to_fit();
}

void TriangleBVH::buildRecursive(uint32_t nodeIdx, uint32_t first, uint32_t count) {
    // Primitive bbox + centroid bbox over [first, first+count).
    BoundingBox pbox = emptyBBox();
    BoundingBox cbox = emptyBBox();
    for (uint32_t i = first; i < first + count; i++) {
        expandBBox(pbox, prims[i]->bbox);
        expandBBox(cbox, centroids[i]);
    }
    nodes[nodeIdx].bmin = pbox.min;
    nodes[nodeIdx].bmax = pbox.max;

    if (count <= BVH_LEAF_SIZE) {
        nodes[nodeIdx].firstOrLeft = first;
        nodes[nodeIdx].count = (uint16_t)count;
        nodes[nodeIdx].axis = 0;
        return;
    }

    // Pick the axis with the widest centroid extent — this is where binning
    // can make the finest distinction between primitives.
    const vec3 cext = cbox.max - cbox.min;
    int axis = 0;
    if (cext.y > cext.x) axis = 1;
    if (cext.z > ((axis == 0) ? cext.x : cext.y)) axis = 2;

    const float cmin = cbox.min[axis];
    const float cextAxis = cext[axis];
    if (cextAxis < 1e-12f) {
        // All centroids coincide; can't split meaningfully.
        nodes[nodeIdx].firstOrLeft = first;
        nodes[nodeIdx].count = (uint16_t)std::min<uint32_t>(count, 0xFFFFu);
        nodes[nodeIdx].axis = 0;
        return;
    }

    const float scale = (float)BVH_BINS / cextAxis;

    Bin bins[BVH_BINS];
    for (uint32_t i = first; i < first + count; i++) {
        int b = (int)((centroids[i][axis] - cmin) * scale);
        if (b < 0) b = 0;
        if (b >= BVH_BINS) b = BVH_BINS - 1;
        expandBBox(bins[b].bbox, prims[i]->bbox);
        bins[b].count++;
    }

    // Sweep bins left→right and right→left to compute per-split SAH.
    float leftArea[BVH_BINS - 1], rightArea[BVH_BINS - 1];
    int   leftCnt[BVH_BINS - 1],  rightCnt[BVH_BINS - 1];
    {
        BoundingBox lb = emptyBBox();
        int lc = 0;
        for (int i = 0; i < BVH_BINS - 1; i++) {
            if (bins[i].count > 0) expandBBox(lb, bins[i].bbox);
            lc += bins[i].count;
            leftArea[i] = (lc > 0) ? surfaceArea(lb) : 0.0f;
            leftCnt[i]  = lc;
        }
        BoundingBox rb = emptyBBox();
        int rc = 0;
        for (int i = BVH_BINS - 1; i > 0; i--) {
            if (bins[i].count > 0) expandBBox(rb, bins[i].bbox);
            rc += bins[i].count;
            rightArea[i - 1] = (rc > 0) ? surfaceArea(rb) : 0.0f;
            rightCnt[i - 1]  = rc;
        }
    }

    const float parentArea = surfaceArea(pbox);
    const float invParentArea = (parentArea > 0.0f) ? 1.0f / parentArea : 0.0f;

    const float leafCost = (float)count * BVH_INTERSECT_COST;
    float bestCost = leafCost;
    int bestBin = -1;
    for (int i = 0; i < BVH_BINS - 1; i++) {
        if (leftCnt[i] == 0 || rightCnt[i] == 0) continue;
        const float cost = BVH_TRAVERSAL_COST
            + BVH_INTERSECT_COST * invParentArea
              * (leftArea[i] * leftCnt[i] + rightArea[i] * rightCnt[i]);
        if (cost < bestCost) {
            bestCost = cost;
            bestBin = i;
        }
    }

    if (bestBin < 0) {
        // Splitting won't help. Force a leaf even if > BVH_LEAF_SIZE; clamp
        // to uint16 so count fits (scenes with >65k prims in one spot are
        // rare, but be defensive).
        const uint32_t leafCount = std::min<uint32_t>(count, 0xFFFFu);
        nodes[nodeIdx].firstOrLeft = first;
        nodes[nodeIdx].count = (uint16_t)leafCount;
        nodes[nodeIdx].axis = 0;
        return;
    }

    // Partition in place by bin index.
    uint32_t mid = first;
    for (uint32_t i = first; i < first + count; i++) {
        int b = (int)((centroids[i][axis] - cmin) * scale);
        if (b < 0) b = 0;
        if (b >= BVH_BINS) b = BVH_BINS - 1;
        if (b <= bestBin) {
            std::swap(prims[i], prims[mid]);
            std::swap(centroids[i], centroids[mid]);
            mid++;
        }
    }

    const uint32_t leftCount = mid - first;
    const uint32_t rightCount = count - leftCount;
    if (leftCount == 0 || rightCount == 0) {
        const uint32_t leafCount = std::min<uint32_t>(count, 0xFFFFu);
        nodes[nodeIdx].firstOrLeft = first;
        nodes[nodeIdx].count = (uint16_t)leafCount;
        nodes[nodeIdx].axis = 0;
        return;
    }

    // Allocate children contiguously (left at leftIdx, right at leftIdx+1) so
    // we only need one index stored in the parent.
    const uint32_t leftIdx = (uint32_t)nodes.size();
    nodes.emplace_back();
    nodes.emplace_back();

    nodes[nodeIdx].firstOrLeft = leftIdx;
    nodes[nodeIdx].count = 0;
    nodes[nodeIdx].axis = (uint16_t)axis;

    buildRecursive(leftIdx,     first, leftCount);
    buildRecursive(leftIdx + 1, mid,   rightCount);
}

bool TriangleBVH::intersectClosest(const Ray& ray, RayTriangleIntersectionInfo& info) const {
    if (nodes.empty()) return false;

    const float invDx = (fabsf(ray.dir.x) > 1e-20f) ? 1.0f / ray.dir.x : 1e30f;
    const float invDy = (fabsf(ray.dir.y) > 1e-20f) ? 1.0f / ray.dir.y : 1e30f;
    const float invDz = (fabsf(ray.dir.z) > 1e-20f) ? 1.0f / ray.dir.z : 1e30f;
    const int signX = invDx < 0.0f;
    const int signY = invDy < 0.0f;
    const int signZ = invDz < 0.0f;
    const int dirSign[3] = { signX, signY, signZ };

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;

    bool anyHit = false;

    while (sp > 0) {
        const BVHNode& n = nodes[stack[--sp]];

        // Slab test; reject if behind origin or farther than current closest.
        const float t1x = (n.bmin.x - ray.origin.x) * invDx;
        const float t2x = (n.bmax.x - ray.origin.x) * invDx;
        const float t1y = (n.bmin.y - ray.origin.y) * invDy;
        const float t2y = (n.bmax.y - ray.origin.y) * invDy;
        const float t1z = (n.bmin.z - ray.origin.z) * invDz;
        const float t2z = (n.bmax.z - ray.origin.z) * invDz;
        const float tmin = fmaxf(fmaxf(fminf(t1x, t2x), fminf(t1y, t2y)), fminf(t1z, t2z));
        const float tmax = fminf(fminf(fmaxf(t1x, t2x), fmaxf(t1y, t2y)), fmaxf(t1z, t2z));
        if (tmax < 0.0f || tmin > tmax || tmin > info.t) continue;

        if (n.count > 0) {
            for (uint16_t i = 0; i < n.count; i++) {
                if (prims[n.firstOrLeft + i]->intersectsRay(ray, info)) {
                    anyHit = true;
                }
            }
        } else {
            // Push far child first so the near child is popped first — gives
            // early-out on the closer half and tightens info.t faster.
            const uint32_t l = n.firstOrLeft;
            const uint32_t r = l + 1;
            if (dirSign[n.axis]) {
                stack[sp++] = l;
                stack[sp++] = r;
            } else {
                stack[sp++] = r;
                stack[sp++] = l;
            }
        }
    }
    return anyHit;
}

}
