///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __raygen_bvh_h__
#define __raygen_bvh_h__

#include <vector>
#include <cstdint>
#include "ugm/types3d.h"
#include "raycommon.h"

namespace raygen {

// 32-byte BVH node. Leaves have `count > 0` and `firstOrLeft` indexes into
// the primitive array; internal nodes have `count == 0` and `firstOrLeft`
// indexes into the node array (right child is always at firstOrLeft+1).
struct BVHNode {
    ugm::vec3 bmin;         // 12
    uint32_t firstOrLeft;   //  4
    ugm::vec3 bmax;         // 12
    uint16_t count;         //  2  — primitive count in leaf; 0 for internal
    uint16_t axis;          //  2  — split axis for internal nodes (0/1/2)

    inline bool isLeaf() const { return count > 0; }
};

class TriangleBVH {
public:
    void reset();

    // Reorders `prims` in place (permutation); stored pointers are retained.
    void build(std::vector<const RenderMeshTriangle*>& prims);

    // Closest-hit traversal. `info.t` is used as the current closest distance
    // for node/prim pruning and is updated as closer hits are found.
    bool intersectClosest(const ugm::Ray& ray, RayTriangleIntersectionInfo& info) const;

    // Any-hit traversal for shadow/occlusion rays. Returns true as soon as a
    // primitive in (0, maxT) passes `pred`. `pred` is called with a
    // `const RenderMeshTriangle*` after a ray-triangle intersection is
    // confirmed; return true to treat the primitive as an occluder.
    template<typename Pred>
    bool intersectAny(const ugm::Ray& ray, float maxT, Pred&& pred) const;

    inline size_t nodeCount() const { return nodes.size(); }
    inline size_t primCount() const { return prims.size(); }

private:
    std::vector<BVHNode> nodes;
    std::vector<const RenderMeshTriangle*> prims;
    std::vector<ugm::vec3> centroids;  // parallel to prims during build

    void buildRecursive(uint32_t nodeIdx, uint32_t first, uint32_t count);
};

// Implementation of intersectAny kept in the header so the predicate is
// inlined at each call site — the whole point of templating the callback.
template<typename Pred>
bool TriangleBVH::intersectAny(const ugm::Ray& ray, float maxT, Pred&& pred) const {
    if (nodes.empty()) return false;

    const float invDx = (fabsf(ray.dir.x) > 1e-20f) ? 1.0f / ray.dir.x : 1e30f;
    const float invDy = (fabsf(ray.dir.y) > 1e-20f) ? 1.0f / ray.dir.y : 1e30f;
    const float invDz = (fabsf(ray.dir.z) > 1e-20f) ? 1.0f / ray.dir.z : 1e30f;

    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;

    while (sp > 0) {
        const BVHNode& n = nodes[stack[--sp]];

        float t1x = (n.bmin.x - ray.origin.x) * invDx;
        float t2x = (n.bmax.x - ray.origin.x) * invDx;
        float t1y = (n.bmin.y - ray.origin.y) * invDy;
        float t2y = (n.bmax.y - ray.origin.y) * invDy;
        float t1z = (n.bmin.z - ray.origin.z) * invDz;
        float t2z = (n.bmax.z - ray.origin.z) * invDz;
        float tmin = fmaxf(fmaxf(fminf(t1x, t2x), fminf(t1y, t2y)), fminf(t1z, t2z));
        float tmax = fminf(fminf(fmaxf(t1x, t2x), fmaxf(t1y, t2y)), fmaxf(t1z, t2z));
        if (tmax < 0.0f || tmin > tmax || tmin > maxT) continue;

        if (n.count > 0) {
            for (uint16_t i = 0; i < n.count; i++) {
                const RenderMeshTriangle* rt = prims[n.firstOrLeft + i];
                float t;
                ugm::vec3 h;
                if (rt->intersectsRay(ray, maxT, t, h) && pred(rt)) return true;
            }
        } else {
            stack[sp++] = n.firstOrLeft + 1;
            stack[sp++] = n.firstOrLeft;
        }
    }
    return false;
}

}

#endif
