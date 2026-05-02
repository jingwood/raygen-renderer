///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Property panel implementation.
///////////////////////////////////////////////////////////////////////////////

#include "PropertyPanel.h"

#include <cstring>

#include "imgui.h"

#include "raygen/material.h"
#include "raygen/scene.h"
#include "raygen/texture.h"
#include "ucm/string.h"
#include "ugm/color.h"
#include "ugm/vector.h"

#include "Dialog.h"
#include "MediumEditor.h"

namespace raygen {
namespace viewer {

namespace {

// Transform — edits apply instantly to the SceneObject. The renderer
// re-flattens + rebuilds the BVH on every Full render, so a dirty kick is
// enough to see the change.
bool drawTransform(SceneObject& so) {
    if (!ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) return false;

    bool dirty = false;
    float loc[3] = { so.location.x, so.location.y, so.location.z };
    float ang[3] = { so.angle.x,    so.angle.y,    so.angle.z };
    float scl[3] = { so.scale.x,    so.scale.y,    so.scale.z };
    if (ImGui::DragFloat3("location", loc, 0.05f, -1000.0f, 1000.0f, "%.3f")) {
        so.location = ugm::vec3(loc[0], loc[1], loc[2]);
        dirty = true;
    }
    if (ImGui::DragFloat3("angle",    ang, 0.5f,  -360.0f, 360.0f, "%.2f")) {
        so.angle = ugm::vec3(ang[0], ang[1], ang[2]);
        dirty = true;
    }
    if (ImGui::DragFloat3("scale",    scl, 0.01f, 0.0001f, 1000.0f, "%.3f")) {
        so.scale = ugm::vec3(scl[0], scl[1], scl[2]);
        dirty = true;
    }

    bool v = so.visible;
    if (ImGui::Checkbox("visible", &v))    { so.visible = v;    dirty = true; }
    ImGui::SameLine();
    bool r = so.renderable;
    if (ImGui::Checkbox("renderable", &r)) { so.renderable = r; dirty = true; }
    return dirty;
}

// Compute the directory portion of `path` into `outDir` for seeding the
// texture-file dialog at a useful location.
void deriveDirOf(const char* path, char* outDir, size_t outDirCap) {
    outDir[0] = '\0';
    if (path == nullptr || outDirCap == 0) return;
    const char* lsU = std::strrchr(path, '/');
    const char* lsW = std::strrchr(path, '\\');
    const char* ls  = lsU > lsW ? lsU : lsW;
    if (ls == nullptr) return;
    size_t n = (size_t)(ls - path);
    if (n >= outDirCap) n = outDirCap - 1;
    std::memcpy(outDir, path, n);
    outDir[n] = '\0';
}

// "texture: <path> [Browse...] [Clear]" row. `existingPath` may be empty;
// `scenePath` seeds the dialog folder when no texture is set yet. Returns
// true if the user picked a new texture (or cleared); the caller should
// then update `pathField` and `texPtr` accordingly.
bool drawTextureRow(const char* label, ucm::string& pathField,
                    Texture*& texPtr, bool isRendering,
                    const char* scenePath, const char* idSuffix) {
    bool changed = false;
    if (!pathField.isEmpty()) {
        ImGui::TextWrapped("%s: %s", label, pathField.getBuffer());
    } else {
        ImGui::TextDisabled("%s: (none)", label);
    }

    const bool canPick = !isRendering;
    if (!canPick) ImGui::BeginDisabled();
    char btnBrowse[64];
    std::snprintf(btnBrowse, sizeof(btnBrowse), "Browse...##%s", idSuffix);
    if (ImGui::Button(btnBrowse)) {
        char initDir[512];
        deriveDirOf(!pathField.isEmpty() ? pathField.getBuffer() : scenePath,
                    initDir, sizeof(initDir));
        char picked[1024] = {0};
        if (openImageFileDialog(picked, sizeof(picked),
                                "Open texture",
                                initDir[0] ? initDir : nullptr)) {
            ucm::string p(picked);
            Texture* tex = SceneResourcePool::instance.getTexture(p);
            if (tex != nullptr) {
                texPtr    = tex;
                pathField = p;
                changed   = true;
            } else {
                fprintf(stderr, "load texture failed: %s\n", picked);
            }
        }
    }
    if (!pathField.isEmpty()) {
        ImGui::SameLine();
        char btnClear[64];
        std::snprintf(btnClear, sizeof(btnClear), "Clear##%s", idSuffix);
        if (ImGui::Button(btnClear)) {
            // Drop the texture pointer; the resource pool still owns the
            // Texture* (it caches by path) so we don't delete it here.
            texPtr = NULL;
            pathField.clear();
            changed = true;
        }
    }
    if (!canPick) ImGui::EndDisabled();
    return changed;
}

// Material — edits write directly to SceneObject::material. The renderer
// reads material by value per-hit via a pointer on the triangle, so the
// change shows up on the next trace. 32-bit float writes are atomic so
// dragging a slider can't tear a pixel — only briefly mix old/new channels.
bool drawMaterial(Material& m, bool isRendering, const char* scenePath) {
    if (!ImGui::CollapsingHeader("Material", ImGuiTreeNodeFlags_DefaultOpen)) return false;

    bool dirty = false;
    if (!m.name.isEmpty()) {
        ImGui::TextDisabled("shared: %s", m.name.getBuffer());
    }

    float col[3] = { m.color.r, m.color.g, m.color.b };
    if (ImGui::ColorEdit3("color", col)) {
        m.color = ugm::color3(col[0], col[1], col[2]);
        dirty = true;
    }
    dirty |= ImGui::SliderFloat("diffuse",          &m.diffuse,          0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("glossy",           &m.glossy,           0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("metallic",         &m.metallic,         0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("roughness",        &m.roughness,        0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("anisotropy",       &m.anisotropy,      -1.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("anisoRotation",    &m.anisoRotation,    0.0f, 360.0f,  "%.1f");
    dirty |= ImGui::SliderFloat("transparency",     &m.transparency,     0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("refraction",       &m.refraction,       0.0f, 1.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("refractionRatio",  &m.refractionRatio,  1.0f, 3.0f,    "%.3f");
    dirty |= ImGui::SliderFloat("chromaDispersion", &m.chromaDispersion, 0.0f, 0.1f,    "%.4f");
    dirty |= ImGui::DragFloat  ("emission",         &m.emission,         0.1f, 0.0f, 10000.0f, "%.2f");
    dirty |= ImGui::DragFloat  ("spotRange",        &m.spotRange,        0.01f, 0.0f, 100.0f,  "%.3f");

    ImGui::Spacing();
    dirty |= drawTextureRow("texture",    m.texturePath,   m.texture,
                            isRendering,  scenePath, "tex");
    dirty |= drawTextureRow("normal map", m.normalmapPath, m.normalmap,
                            isRendering,  scenePath, "nmap");
    return dirty;
}

void drawHeader(const SceneObject& so) {
    ImGui::Text("name: %s",
                so.getName().isEmpty() ? "(unnamed)"
                                       : so.getName().getBuffer());
    ImGui::Text("meshes: %zu   children: %zu",
                so.getMeshes().size(), so.getObjects().size());
    ImGui::Separator();
}

}  // namespace

bool drawPropertyPanel(const PropertyPanelCtx& ctx) {
    ImGui::Begin("Property");
    bool dirty = false;
    SceneObject* selected = ctx.selected;
    if (!selected) {
        ImGui::TextDisabled("Select an object in the Outline window to inspect.");
    } else {
        drawHeader(*selected);
        dirty |= drawTransform(*selected);
        dirty |= drawMaterial(selected->material, ctx.isRendering, ctx.scenePath);
        // Interior medium UI lives in MediumEditor.cpp — see header for why
        // it's its own file (keeps Phase-by-Phase volume work scoped).
        dirty |= drawInteriorMedium(*selected);
    }
    ImGui::End();
    return dirty;
}

}  // namespace viewer
}  // namespace raygen
