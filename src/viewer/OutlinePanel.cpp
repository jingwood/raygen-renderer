///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Outline panel implementation.
///////////////////////////////////////////////////////////////////////////////

#include "OutlinePanel.h"

#include "imgui.h"

#include "raygen/scene.h"
#include "ucm/string.h"

namespace raygen {
namespace viewer {

namespace {

// Recursive draw for the Outline tree. Any mutation of visibility happens
// directly on the SceneObject — ImGui edit widgets return true whenever the
// user dragged, which drives `dirty`. The tree is drawn every frame so new
// children (post-Reload) appear automatically.
void drawObjectNode(SceneObject* obj, SceneObject*& selected, bool& dirty) {
    if (!obj) return;
    const auto& children = obj->getObjects();
    const bool leaf = children.empty();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (leaf) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (obj == selected) flags |= ImGuiTreeNodeFlags_Selected;

    // Visibility checkbox on the left; ##<addr> keeps IDs unique even when
    // two siblings share a display name.
    ImGui::PushID((void*)obj);
    bool visible = obj->visible;
    if (ImGui::Checkbox("##vis", &visible)) {
        obj->visible = visible;
        dirty = true;
    }
    ImGui::SameLine();

    const char* label = obj->getName().isEmpty() ? "(unnamed)"
                                                 : obj->getName().getBuffer();
    bool opened = ImGui::TreeNodeEx((void*)obj, flags, "%s", label);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        selected = obj;
    }
    ImGui::PopID();

    if (opened && !leaf) {
        for (SceneObject* child : children) {
            drawObjectNode(child, selected, dirty);
        }
        ImGui::TreePop();
    }
}

}  // namespace

bool drawOutlinePanel(Scene& scene, SceneObject*& selected) {
    bool dirty = false;
    // Default layout: right column, top ~28% of height.
    {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos( ImVec2(0.734f * ds.x, 0.016f * ds.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(0.256f * ds.x, 0.282f * ds.y), ImGuiCond_FirstUseEver);
    }
    ImGui::Begin("Outline");
    for (SceneObject* root : scene.getObjects()) {
        drawObjectNode(root, selected, dirty);
    }
    ImGui::End();
    return dirty;
}

}  // namespace viewer
}  // namespace raygen
