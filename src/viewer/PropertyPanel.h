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

// Renders the "Property" window. `selected` may be null (panel just shows a
// hint). Returns true when any field changed so the caller can mark the
// scene dirty and route a Full re-render.
bool drawPropertyPanel(SceneObject* selected);

}  // namespace viewer
}  // namespace raygen

#endif
