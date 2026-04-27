///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Property panel implementation.
///////////////////////////////////////////////////////////////////////////////

#include "PropertyPanel.h"

#include "imgui.h"

#include "raygen/material.h"
#include "raygen/scene.h"
#include "ucm/string.h"
#include "ugm/color.h"
#include "ugm/vector.h"

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

// Material — edits write directly to SceneObject::material. The renderer
// reads material by value per-hit via a pointer on the triangle, so the
// change shows up on the next trace. 32-bit float writes are atomic so
// dragging a slider can't tear a pixel — only briefly mix old/new channels.
bool drawMaterial(Material& m) {
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

    if (!m.texturePath.isEmpty())
        ImGui::TextDisabled("texture:    %s", m.texturePath.getBuffer());
    if (!m.normalmapPath.isEmpty())
        ImGui::TextDisabled("normal map: %s", m.normalmapPath.getBuffer());
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

bool drawPropertyPanel(SceneObject* selected) {
    ImGui::Begin("Property");
    bool dirty = false;
    if (!selected) {
        ImGui::TextDisabled("Select an object in the Outline window to inspect.");
    } else {
        drawHeader(*selected);
        dirty |= drawTransform(*selected);
        dirty |= drawMaterial(selected->material);
        // Interior medium UI lives in MediumEditor.cpp — see header for why
        // it's its own file (keeps Phase-by-Phase volume work scoped).
        dirty |= drawInteriorMedium(*selected);
    }
    ImGui::End();
    return dirty;
}

}  // namespace viewer
}  // namespace raygen
