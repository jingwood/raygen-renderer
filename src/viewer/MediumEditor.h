///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Property panel: Interior medium editor.
//
//  Split out of main.cpp because the participating-media UI keeps growing
//  one Phase at a time (P1 σ, P2 emission, P2.5 cone, P3 fBm, P4 NEE...) —
//  isolating it here keeps PropertyPanel focused on Transform/Material and
//  scopes future medium work to one file.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_medium_editor_h__
#define __viewer_medium_editor_h__

namespace raygen { class SceneObject; }

namespace raygen {
namespace viewer {

// Renders the "Interior medium" CollapsingHeader for the given object.
// Owns the enable-checkbox lifecycle (allocates/frees the medium) and the
// nested mode pickers (emission Constant/Cone, density None/fBm).
// Returns true when any value changed so the caller can mark the scene dirty
// and trigger a Full re-render.
bool drawInteriorMedium(SceneObject& so);

}  // namespace viewer
}  // namespace raygen

#endif
