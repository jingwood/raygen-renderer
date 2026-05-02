///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Property panel (inspector for the selected SceneObject).
//
//  Lays out three CollapsingHeader sections: Transform (location/angle/scale,
//  visible/renderable), Material (color + lobe sliders), Interior medium
//  (delegated to MediumEditor.cpp). Pulled out of main.cpp so material
//  authoring features can grow without crowding the worker-thread / UI loop.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_property_panel_h__
#define __viewer_property_panel_h__

namespace raygen { class SceneObject; }

namespace raygen {
namespace viewer {

struct PropertyPanelCtx {
    // Selected SceneObject, may be null (panel just shows a hint).
    SceneObject* selected;

    // Disables texture-load buttons while a render is in flight, since the
    // worker reads Material::texture per-hit and the swap happens on the main
    // thread.
    bool isRendering;

    // Used to seed the texture-file dialog at the scene folder when the
    // material has no current texture. May be null.
    const char* scenePath;
};

// Renders the "Property" window. Returns true when any field changed so the
// caller can mark the scene dirty and route a Full re-render.
bool drawPropertyPanel(const PropertyPanelCtx& ctx);

}  // namespace viewer
}  // namespace raygen

#endif
