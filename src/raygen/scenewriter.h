///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef __scene_writer_h__
#define __scene_writer_h__

#include <map>

#include "ucm/string.h"
#include "ucm/archive.h"
#include "ucm/jstypes.h"
#include "ugm/image.h"

#include "scene.h"
#include "mesh.h"
#include "material.h"
#include "medium.h"

namespace raygen {

// Serializes a Scene back to JSON. Two modes:
//
//   1. External — `bundle == NULL`: mesh and texture references are emitted as
//      original disk paths. Suitable for "Save As scene.json" dump where the
//      user can hand-edit the result.
//   2. Bundle  — `bundle != NULL`: mesh and texture references resolve into
//      chunks already inserted by SceneBundleSaver, producing
//      `tob://__this__/<uid>` URIs that the existing loader handles.
//
// The JSON shape mirrors what SceneJsonLoader reads, so the round-trip is
// schema-stable: no new keys, no migration. JSONC comments and authored
// formatting from the source scene file are *not* preserved — Save replaces
// the file with a freshly serialized tree.
class SceneJsonWriter {
public:
    // Returns the manifest JSON for the scene. When `bundle` is non-NULL,
    // SceneBundleSaver has already populated `meshUids` / `textureUids` for
    // shared resource references; the writer only emits URIs and does not
    // touch the archive itself.
    static string writeJson(const Scene& scene,
                            ucm::Archive* bundle = NULL,
                            const std::map<const Mesh*, uint>* meshUids = NULL,
                            const std::map<string, uint>* textureUids = NULL);
};

// Packages the current Scene into a single .toba archive:
//
//   chunk uid=1, format=MIFT — manifest JSON describing the scene tree
//   chunk uid=2, format=PNG  — optional thumbnail (small tonemapped preview)
//   chunk uid>=3             — meshes (FORMAT_TAG_MESH) and textures
//                              (FORMAT_TAG_JPEG / FORMAT_TAG_PNG / etc.)
//
// Designed to be the inverse of SceneJsonLoader::loadBundle(): the produced
// file should round-trip without further hand editing. Mesh sharing and
// texture path dedup are handled internally so a chair authored once is
// embedded once even if 50 SceneObjects reference the same Mesh*.
//
// Textures are copied byte-for-byte from disk when possible (preserves the
// original JPEG / PNG quality and avoids a re-encode round-trip). HDR or
// missing files fall back to keeping the absolute path on the material —
// the bundle is not strictly self-contained in that case.
class SceneBundleSaver {
public:
    // Writes `scene` to `path`. Optional `thumbnail` is encoded as PNG into
    // chunk uid=2; pass NULL to skip thumbnail generation. Throws on archive
    // I/O failure (matches Archive::save's contract).
    static void save(const Scene& scene,
                     const string& path,
                     const Image* thumbnail = NULL);
};

}

#endif /* __scene_writer_h__ */
