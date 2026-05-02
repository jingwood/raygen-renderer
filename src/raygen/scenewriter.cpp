///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "scenewriter.h"

#include <cmath>

#include "ucm/file.h"
#include "ucm/stream.h"
#include "ucm/jsonwriter.h"
#include "ugm/imgcodec.h"

#include "meshloader.h"

// Manifest format tag mirrors sceneloader.cpp's private constant: little-
// endian "mift" in ASCII. Re-declared here rather than exported because the
// writer is the only other place that needs it.
#define FORMAT_TAG_MIFT 0x7466696d
// Radiance .hdr (RGBE) FourCC. ugm/imgcodec.h has the same constant under a
// shared namespace; duplicating here keeps scenewriter.cpp from leaking that
// header into raygen's public surface.
#define FORMAT_TAG_HDR  0x20726468

namespace raygen {

using ucm::Archive;
using ucm::ChunkEntry;
using ucm::JSONWriter;
using ucm::JSObject;
using ucm::JSValue;
using ucm::JSType;
using ucm::FileStream;
using ucm::FileStreamType;
using ucm::Stream;
using ucm::MemoryStream;

namespace {

// Approximate float equality for "did the user touch the default?" checks.
// Material defaults are exact constants so equality works in the common case;
// the epsilon catches drift from UI sliders that step in non-binary
// increments (e.g. ImGui::DragFloat with a 0.01 step).
inline bool feq(float a, float b) {
    const float d = a - b;
    return d < 1e-6f && d > -1e-6f;
}

inline bool vec3IsZero(const vec3& v) {
    return feq(v.x, 0.0f) && feq(v.y, 0.0f) && feq(v.z, 0.0f);
}
inline bool vec3IsOne(const vec3& v) {
    return feq(v.x, 1.0f) && feq(v.y, 1.0f) && feq(v.z, 1.0f);
}
inline bool color3IsZero(const color3& c) {
    return feq(c.r, 0.0f) && feq(c.g, 0.0f) && feq(c.b, 0.0f);
}
inline bool color3IsDefaultGrey(const color3& c) {
    return feq(c.r, 0.8f) && feq(c.g, 0.8f) && feq(c.b, 0.8f);
}

// FourCC the loader's archive image decoder probes for. Defaults to JPEG when
// the extension is unknown — `loadImage(archive, uid, ICF_AUTO)` already
// auto-tries each codec, so a wrong tag only adds one failed probe. The HDR
// tag is special-cased: the bundle loader looks for it explicitly to route
// to the RGBE decoder (auto-detect doesn't try HDR).
uint formatTagFromTexturePath(const string& path) {
    if (path.endsWith(".png",  ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_PNG;
    if (path.endsWith(".bmp",  ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_BMP;
    if (path.endsWith(".gif",  ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_GIF;
    if (path.endsWith(".jpg",  ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_JPEG;
    if (path.endsWith(".jpeg", ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_JPEG;
    if (path.endsWith(".hdr",  ucm::StringComparingFlags::SCF_CASE_INSENSITIVE)) return FORMAT_TAG_HDR;
    return FORMAT_TAG_JPEG;
}

// Copy raw bytes from `srcPath` into `dst`. Preserves the original codec so a
// JPEG stays a JPEG (no lossy re-encode). Returns false if the source can't
// be opened — callers fall back to keeping the disk path on the material so
// the bundle still loads, just not self-contained.
bool copyFileIntoChunk(const string& srcPath, MemoryStream& dst) {
    try {
        FileStream fs(srcPath);
        fs.openRead(FileStreamType::Binary);
        Stream::copy(fs, dst);
        fs.close();
        return dst.getLength() > 0;
    } catch (...) {
        return false;
    }
}

// Walk a JSObject we built up and free anything the destructor leaves
// behind. Specifically: JSValue elements stored inside JSValue arrays — the
// vector's destructor runs ~JSValue() which is empty, so nested JSObject*
// and string* members leak unless we free them ourselves. (JSObject's own
// destructor handles direct properties; this only covers array elements.)
void freeJsArrayContents(std::vector<JSValue>* arr) {
    if (arr == NULL) return;
    for (JSValue& v : *arr) {
        if (v.type == JSType::JSType_Object && v.object != NULL) {
            // Recurse — the inner JSObject's destructor takes care of its
            // own properties, including any nested arrays.
            for (const auto& p : v.object->getProperties()) {
                if (p.second.type == JSType::JSType_Array) {
                    freeJsArrayContents(p.second.array);
                }
            }
            delete v.object;
            v.object = NULL;
        } else if (v.type == JSType::JSType_String && v.str != NULL) {
            delete v.str;
            v.str = NULL;
        } else if (v.type == JSType::JSType_Array && v.array != NULL) {
            freeJsArrayContents(v.array);
            delete v.array;
            v.array = NULL;
        }
    }
    arr->clear();
}

// Free leaks left by JSObject's destructor inside any of `root`'s nested
// arrays before deleting the root itself.
void freeJsObjectArrays(JSObject* root) {
    if (root == NULL) return;
    for (const auto& p : root->getProperties()) {
        if (p.second.type == JSType::JSType_Array) {
            freeJsArrayContents(p.second.array);
        } else if (p.second.type == JSType::JSType_Object && p.second.object != NULL) {
            freeJsObjectArrays(p.second.object);
        }
    }
}

// ---- JSValue array constructors -------------------------------------------
JSValue jsVec3Array(const vec3& v) {
    JSValue arr(new std::vector<JSValue>());
    arr.array->push_back(JSValue((double)v.x));
    arr.array->push_back(JSValue((double)v.y));
    arr.array->push_back(JSValue((double)v.z));
    return arr;
}
JSValue jsVec2Array(const vec2& v) {
    JSValue arr(new std::vector<JSValue>());
    arr.array->push_back(JSValue((double)v.x));
    arr.array->push_back(JSValue((double)v.y));
    return arr;
}
JSValue jsColor3Array(const color3& c) {
    JSValue arr(new std::vector<JSValue>());
    arr.array->push_back(JSValue((double)c.r));
    arr.array->push_back(JSValue((double)c.g));
    arr.array->push_back(JSValue((double)c.b));
    return arr;
}

// ---- Builders -------------------------------------------------------------

JSObject* buildMaterialJS(const Material& m,
                          const std::map<string, uint>* textureUids) {
    JSObject* o = new JSObject();
    if (!color3IsDefaultGrey(m.color))   o->setProperty("color", jsColor3Array(m.color));
    if (!feq(m.diffuse, 0.8f))           o->setProperty("diffuse",          (double)m.diffuse);
    if (!feq(m.glossy, 0.0f))            o->setProperty("glossy",           (double)m.glossy);
    if (!feq(m.roughness, 0.5f))         o->setProperty("roughness",        (double)m.roughness);
    if (!feq(m.transparency, 0.0f))      o->setProperty("transparency",     (double)m.transparency);
    if (!feq(m.refraction, 0.0f))        o->setProperty("refraction",       (double)m.refraction);
    if (!feq(m.refractionRatio, 1.45f))  o->setProperty("refractionRatio",  (double)m.refractionRatio);
    if (!feq(m.chromaDispersion, 0.0f))  o->setProperty("chromaDispersion", (double)m.chromaDispersion);
    if (!feq(m.metallic, 0.0f))          o->setProperty("metallic",         (double)m.metallic);
    if (!feq(m.anisotropy, 0.0f))        o->setProperty("anisotropy",       (double)m.anisotropy);
    if (!feq(m.anisoRotation, 0.0f))     o->setProperty("anisoRotation",    (double)m.anisoRotation);
    if (!feq(m.emission, 0.0f))          o->setProperty("emission",         (double)m.emission);
    if (!feq(m.spotRange, 0.0f))         o->setProperty("spotRange",        (double)m.spotRange);
    if (!feq(m.normalMipmap, 0.0f))      o->setProperty("normalMipmap",     (double)m.normalMipmap);
    if (!feq(m.texTiling.x, 1.0f) || !feq(m.texTiling.y, 1.0f)) {
        o->setProperty("texTiling", jsVec2Array(m.texTiling));
    }
    if (!m.texturePath.isEmpty()) {
        // In bundle mode, texture chunks were inserted ahead of time; fall
        // back to the disk path when the chunk wasn't created (HDR / missing
        // file) so the scene at least loads.
        if (textureUids != NULL) {
            const auto it = textureUids->find(m.texturePath);
            if (it != textureUids->end()) {
                string uri;
                uri.appendFormat("tob://__this__/%08x", it->second);
                o->setProperty("tex", uri);
            } else {
                o->setProperty("tex", m.texturePath);
            }
        } else {
            o->setProperty("tex", m.texturePath);
        }
    }
    if (!m.normalmapPath.isEmpty()) {
        o->setProperty("normalmap", m.normalmapPath);
    }
    return o;
}

JSObject* buildMediumJS(const HomogeneousMedium& m) {
    JSObject* o = new JSObject();

    if (!color3IsZero(m.sigma_a)) o->setProperty("sigma_a",  jsColor3Array(m.sigma_a));
    if (!color3IsZero(m.sigma_s)) o->setProperty("sigma_s",  jsColor3Array(m.sigma_s));
    if (!color3IsZero(m.sigma_e)) o->setProperty("emission", jsColor3Array(m.sigma_e));
    if (!feq(m.g, 0.0f))          o->setProperty("g",       (double)m.g);
    if (!feq(m.density, 1.0f))    o->setProperty("density", (double)m.density);

    switch (m.emissionMode) {
        case HomogeneousMedium::EmissionMode_Cone: o->setProperty("emissionMode", string("cone")); break;
        case HomogeneousMedium::EmissionMode_Path: o->setProperty("emissionMode", string("path")); break;
        default: break;
    }

    if (m.emissionMode == HomogeneousMedium::EmissionMode_Cone) {
        if (!vec3IsZero(m.coneOrigin)) o->setProperty("coneOrigin", jsVec3Array(m.coneOrigin));
        if (!feq(m.coneAxis.x, 0.0f) || !feq(m.coneAxis.y, 0.0f) || !feq(m.coneAxis.z, -1.0f)) {
            o->setProperty("coneAxis", jsVec3Array(m.coneAxis));
        }
        if (!feq(m.coneLength, 1.0f))         o->setProperty("coneLength",          (double)m.coneLength);
        if (!feq(m.coneRadius, 0.3f))         o->setProperty("coneRadius",          (double)m.coneRadius);
        o->setProperty("coneInner", jsColor3Array(m.coneInner));
        o->setProperty("coneOuter", jsColor3Array(m.coneOuter));
        if (!feq(m.coneIntensity, 50.0f))     o->setProperty("coneIntensity",       (double)m.coneIntensity);
        if (!feq(m.conePeakAxial, 0.15f))     o->setProperty("conePeakAxial",       (double)m.conePeakAxial);
        if (!feq(m.conePeakSharpness, 5.0f))  o->setProperty("conePeakSharpness",   (double)m.conePeakSharpness);
        if (m.coneEmissionSamples != 4)       o->setProperty("coneEmissionSamples", (double)m.coneEmissionSamples);
        if (m.coneFollowObject)               o->setProperty("coneFollowObject",    true);
    }

    if (m.emissionMode == HomogeneousMedium::EmissionMode_Path) {
        o->setProperty("pathInner", jsColor3Array(m.pathInner));
        o->setProperty("pathOuter", jsColor3Array(m.pathOuter));
        if (!feq(m.pathIntensity, 1.0f))      o->setProperty("pathIntensity",       (double)m.pathIntensity);
        if (!feq(m.pathFalloffPower, 2.0f))   o->setProperty("pathFalloffPower",    (double)m.pathFalloffPower);
        if (m.pathEmissionSamples != 6)       o->setProperty("pathEmissionSamples", (double)m.pathEmissionSamples);
        if (m.pathFollowObject)               o->setProperty("pathFollowObject",    true);

        // pathPoints — array of {p, radius, t} objects. JSObject's destructor
        // doesn't free objects nested inside arrays, so freeJsObjectArrays()
        // is run before we delete the root.
        if (!m.pathPoints.empty()) {
            JSValue arr(new std::vector<JSValue>());
            for (const auto& s : m.pathPoints) {
                JSObject* po = new JSObject();
                po->setProperty("p",      jsVec3Array(s.p));
                po->setProperty("radius", (double)s.radius);
                po->setProperty("t",      (double)s.t);
                arr.array->push_back(JSValue(po));
            }
            o->setProperty("pathPoints", arr);
        }
    }

    if (m.densityField == HomogeneousMedium::DensityField_FBmNoise) {
        o->setProperty("densityField", string("fbm"));
        if (!feq(m.noiseFrequency, 1.0f))   o->setProperty("noiseFrequency",  (double)m.noiseFrequency);
        if (m.noiseOctaves != 4)            o->setProperty("noiseOctaves",    (double)m.noiseOctaves);
        if (!feq(m.noiseGain, 0.5f))        o->setProperty("noiseGain",       (double)m.noiseGain);
        if (!feq(m.noiseLacunarity, 2.0f))  o->setProperty("noiseLacunarity", (double)m.noiseLacunarity);
        if (!feq(m.noiseAmplitude, 1.0f))   o->setProperty("noiseAmplitude",  (double)m.noiseAmplitude);
        if (!feq(m.noiseBias, 0.0f))        o->setProperty("noiseBias",       (double)m.noiseBias);
        if (!vec3IsZero(m.noiseOffset))     o->setProperty("noiseOffset", jsVec3Array(m.noiseOffset));
    }

    if (m.heatHaze) {
        o->setProperty("heatHaze", true);
        if (!feq(m.iorAmplitude, 0.005f))   o->setProperty("iorAmplitude",   (double)m.iorAmplitude);
        if (!feq(m.iorFrequency, 4.0f))     o->setProperty("iorFrequency",   (double)m.iorFrequency);
        if (m.iorOctaves != 3)              o->setProperty("iorOctaves",     (double)m.iorOctaves);
        if (!feq(m.iorGain, 0.5f))          o->setProperty("iorGain",        (double)m.iorGain);
        if (!feq(m.iorLacunarity, 2.0f))    o->setProperty("iorLacunarity",  (double)m.iorLacunarity);
        if (m.iorMarchSteps != 16)          o->setProperty("iorMarchSteps",  (double)m.iorMarchSteps);
        if (!vec3IsZero(m.iorOffset))       o->setProperty("iorOffset", jsVec3Array(m.iorOffset));
    }

    return o;
}

string makeUniqueChildKey(const SceneObject& child, int index, std::map<string, int>& used) {
    string base = child.getName();
    if (base.isEmpty()) {
        base = "child";
        base.appendFormat("_%d", index);
    }
    if (used.find(base) == used.end()) {
        used[base] = 1;
        return base;
    }
    int& seen = used[base];
    while (true) {
        string cand = base;
        cand.appendFormat("_%d", ++seen);
        if (used.find(cand) == used.end()) {
            used[cand] = 1;
            return cand;
        }
    }
}

JSObject* buildObjectJS(const SceneObject& obj,
                        const std::map<const Mesh*, uint>* meshUids,
                        const std::map<string, uint>* textureUids) {
    JSObject* o = new JSObject();

    if (!vec3IsZero(obj.location)) o->setProperty("location", jsVec3Array(obj.location));
    if (!vec3IsZero(obj.angle))    o->setProperty("angle",    jsVec3Array(obj.angle));
    if (!vec3IsOne(obj.scale))     o->setProperty("scale",    jsVec3Array(obj.scale));
    if (!obj.visible)              o->setProperty("visible", false);

    // Camera-specific fields go onto the same object — the loader keys camera
    // construction off the JSON key being "mainCamera", so the type is set by
    // where this object lives in the parent map, not by a marker field.
    if (const Camera* cam = dynamic_cast<const Camera*>(&obj)) {
        if (!feq(cam->fieldOfView, 75.0f))      o->setProperty("fieldOfView",      (double)cam->fieldOfView);
        if (!feq(cam->depthOfField, 0.0f))      o->setProperty("depthOfField",     (double)cam->depthOfField);
        if (!feq(cam->aperture, 1.8f))          o->setProperty("aperture",         (double)cam->aperture);
        if (cam->apertureBlades != 0)           o->setProperty("apertureBlades",   (double)cam->apertureBlades);
        if (!feq(cam->apertureRotation, 0.0f))  o->setProperty("apertureRotation", (double)cam->apertureRotation);
        if (!feq(cam->exposure, 1.0f))          o->setProperty("exposure",         (double)cam->exposure);
        if (!cam->focusOnObjectName.isEmpty())  o->setProperty("focusOn",          cam->focusOnObjectName);
    }

    if (!obj.meshes.empty() && meshUids != NULL) {
        const auto it = meshUids->find(obj.meshes[0]);
        if (it != meshUids->end()) {
            string uri;
            uri.appendFormat("tob://__this__/%08x", it->second);
            o->setProperty("mesh", uri);
        }
    }

    // buildMaterialJS / buildMediumJS skip every default-valued field, so
    // emitting `"mat": {}` would just clutter the manifest. Drop the property
    // when the body comes back empty — happens when a SceneObject has the
    // baseline material and no overrides (camera and root container nodes).
    if (obj.material != Material()) {
        JSObject* matJs = buildMaterialJS(obj.material, textureUids);
        if (matJs->getPropertyCount() > 0) {
            o->setProperty("mat", matJs);
        } else {
            delete matJs;
        }
    }

    if (obj.interiorMedium != NULL && obj.interiorMedium->isActive()) {
        JSObject* medJs = buildMediumJS(*obj.interiorMedium);
        if (medJs->getPropertyCount() > 0) {
            o->setProperty("medium", medJs);
        } else {
            delete medJs;
        }
    }

    int childIndex = 0;
    std::map<string, int> usedKeys;
    for (const SceneObject* child : obj.objects) {
        if (child == NULL) continue;
        const bool isMainCamera = (dynamic_cast<const Camera*>(child) != NULL)
                                  && (child->getName() == "mainCamera");
        const string key = isMainCamera ? string("mainCamera")
                                        : makeUniqueChildKey(*child, childIndex++, usedKeys);
        if (isMainCamera) usedKeys[key] = 1;
        o->setProperty(key, buildObjectJS(*child, meshUids, textureUids));
    }
    return o;
}

JSObject* buildSceneJS(const Scene& scene,
                       const std::map<const Mesh*, uint>* meshUids,
                       const std::map<string, uint>* textureUids) {
    JSObject* root = new JSObject();

    int childIndex = 0;
    std::map<string, int> usedKeys;
    for (const SceneObject* child : scene.getObjects()) {
        if (child == NULL) continue;
        const bool isMainCamera = (dynamic_cast<const Camera*>(child) != NULL)
                                  && (child->getName() == "mainCamera");
        const string key = isMainCamera ? string("mainCamera")
                                        : makeUniqueChildKey(*child, childIndex++, usedKeys);
        if (isMainCamera) usedKeys[key] = 1;
        root->setProperty(key, buildObjectJS(*child, meshUids, textureUids));
    }

    // Envmap: emit intensity / rotation, and a `texture` reference when we
    // know the source path. In bundle mode the path is rewritten to a
    // `tob://__this__/<uid>` URI keyed by the same path lookup the material
    // path uses (textureUids was populated for the envmap by the saver
    // before this writer ran). Cubemap blocks aren't round-tripped — the
    // 6-face directory layout has nowhere natural to live inside one chunk.
    if (scene.envmap != NULL || !feq(scene.envmapIntensity, 1.0f) || !feq(scene.envmapRotation, 0.0f) || !scene.envmapPath.isEmpty()) {
        JSObject* env = new JSObject();
        if (!scene.envmapPath.isEmpty()) {
            if (textureUids != NULL) {
                const auto it = textureUids->find(scene.envmapPath);
                if (it != textureUids->end()) {
                    string uri;
                    uri.appendFormat("tob://__this__/%08x", it->second);
                    env->setProperty("texture", uri);
                } else {
                    env->setProperty("texture", scene.envmapPath);
                }
            } else {
                env->setProperty("texture", scene.envmapPath);
            }
        }
        if (!feq(scene.envmapIntensity, 1.0f)) env->setProperty("intensity", (double)scene.envmapIntensity);
        if (!feq(scene.envmapRotation, 0.0f))  env->setProperty("rotation",  (double)scene.envmapRotation);
        root->setProperty("envmap", env);
    }

    if (scene.globalMedium != NULL && scene.globalMedium->isActive()) {
        root->setProperty("medium", buildMediumJS(*scene.globalMedium));
    }

    return root;
}

}  // namespace

string SceneJsonWriter::writeJson(const Scene& scene,
                                  ucm::Archive* /*bundle*/,
                                  const std::map<const Mesh*, uint>* meshUids,
                                  const std::map<string, uint>* textureUids) {
    JSObject* root = buildSceneJS(scene, meshUids, textureUids);

    JSONWriter w;
    w.writeObject(*root);
    string out = w.getString();

    // JSObject's destructor walks direct properties only; sweep array
    // contents (pathPoints, etc.) so nested JSObjects don't leak.
    freeJsObjectArrays(root);
    delete root;

    return out;
}

// ---- SceneBundleSaver -----------------------------------------------------

namespace {

// Walk the tree once, registering meshes and textures with the archive so
// the JSON pass can emit `tob://__this__/<uid>` URIs. Mesh sharing is
// honoured (one chunk per unique Mesh*); texture dedup is keyed by source
// path string, mirroring the loader's SceneResourcePool::textures map.
void collectAndEmbed(const SceneObject& obj,
                     Archive& archive,
                     std::map<const Mesh*, uint>& meshUids,
                     std::map<string, uint>& textureUids) {
    // Material textures: copy raw file bytes so the original codec's quality
    // is preserved. HDR is skipped because the archive-based loadImage
    // doesn't probe FORMAT_TAG_HDR — embedding would create a chunk the
    // loader can't read.
    if (!obj.material.texturePath.isEmpty()) {
        const string& path = obj.material.texturePath;
        if (textureUids.find(path) == textureUids.end()) {
            const bool isHDR = path.endsWith(".hdr", ucm::StringComparingFlags::SCF_CASE_INSENSITIVE);
            // sob:// / tob:// URIs come from a previously-loaded bundle; the
            // image isn't a disk file we can copyFileIntoChunk(). For now
            // leave the URI on the manifest — works only when the user saves
            // back to the same source bundle. A future iteration can pull
            // the bytes through Archive::openChunk and re-embed.
            const bool isBundleUri = path.startsWith("sob://") || path.startsWith("tob://");
            if (!isHDR && !isBundleUri) {
                ChunkEntry* entry = archive.newChunk(formatTagFromTexturePath(path));
                entry->isCompressed = false;  // already compressed (JPEG/PNG)
                if (copyFileIntoChunk(path, *entry->stream)) {
                    const uint uid = entry->uid;
                    archive.updateAndCloseChunk(entry);
                    textureUids[path] = uid;
                } else {
                    // Couldn't read the source — drop the empty placeholder
                    // so it doesn't bloat the archive, and let the manifest
                    // fall back to the disk path on this material.
                    const uint uid = entry->uid;
                    const uint fmt = entry->format;
                    archive.closeChunk(entry);
                    archive.deleteChunk(uid, fmt);
                }
            }
        }
    }

    for (Mesh* mesh : obj.meshes) {
        if (mesh == NULL) continue;
        if (meshUids.find(mesh) != meshUids.end()) continue;
        const uint uid = MeshLoader::save(*mesh, archive);
        meshUids[mesh] = uid;
    }

    for (const SceneObject* child : obj.objects) {
        if (child != NULL) collectAndEmbed(*child, archive, meshUids, textureUids);
    }
}

}  // namespace

void SceneBundleSaver::save(const Scene& scene,
                            const string& path,
                            const Image* thumbnail) {
    Archive archive;

    // Reserve uid=1 (manifest) and uid=2 (thumbnail) before any other newChunk
    // call. FileTrunk's sequential allocator skips taken uids, so subsequent
    // mesh / texture chunks land at 3+. touchChunk also records the format
    // tag so the loader's openChunk(uid, fmt) finds them.
    archive.touchChunk(1, FORMAT_TAG_MIFT);
    archive.touchChunk(2, FORMAT_TAG_PNG);

    std::map<const Mesh*, uint> meshUids;
    std::map<string, uint>      textureUids;

    for (const SceneObject* child : scene.getObjects()) {
        if (child != NULL) collectAndEmbed(*child, archive, meshUids, textureUids);
    }

    // Envmap: embed the source bytes when the scene tracked a disk path. HDR
    // is included this time (unlike material textures) — the bundle loader
    // routes FORMAT_TAG_HDR chunks through the RGBE stream decoder. URIs
    // already pulled in from a previous bundle (sob:// / tob://) are skipped
    // since there's no source file to read from disk.
    if (!scene.envmapPath.isEmpty()
            && !scene.envmapPath.startsWith("sob://")
            && !scene.envmapPath.startsWith("tob://")) {
        if (textureUids.find(scene.envmapPath) == textureUids.end()) {
            ChunkEntry* entry = archive.newChunk(formatTagFromTexturePath(scene.envmapPath));
            entry->isCompressed = false;
            if (copyFileIntoChunk(scene.envmapPath, *entry->stream)) {
                const uint uid = entry->uid;
                archive.updateAndCloseChunk(entry);
                textureUids[scene.envmapPath] = uid;
            } else {
                const uint uid = entry->uid;
                const uint fmt = entry->format;
                archive.closeChunk(entry);
                archive.deleteChunk(uid, fmt);
            }
        }
    }

    string manifest = SceneJsonWriter::writeJson(scene, &archive, &meshUids, &textureUids);
    archive.setTextChunkData(1, FORMAT_TAG_MIFT, manifest);

    if (thumbnail != NULL && thumbnail->width() > 0 && thumbnail->height() > 0) {
        ChunkEntry* entry = archive.openChunk(2, FORMAT_TAG_PNG);
        entry->isCompressed = false;  // PNG is already compressed
        ugm::saveImage(*thumbnail, *entry->stream, ugm::ImageCodecFormat::ICF_PNG);
        archive.updateAndCloseChunk(entry);
    } else {
        // Drop the placeholder so the file doesn't carry a 0-byte chunk.
        archive.deleteChunk(2, FORMAT_TAG_PNG);
    }

    archive.save(path);
}

}

#undef FORMAT_TAG_MIFT
