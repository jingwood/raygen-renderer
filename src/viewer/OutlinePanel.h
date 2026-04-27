///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Outline panel (recursive scene-graph tree).
//
//  Renders the "Outline" window: a tree of every SceneObject under the Scene
//  root, with a per-row visibility checkbox and click-to-select. The selected
//  object is what the Property panel inspects, so this panel owns the
//  selection cursor while the panel below reads from it.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_outline_panel_h__
#define __viewer_outline_panel_h__

namespace raygen {
class Scene;
class SceneObject;
}

namespace raygen {
namespace viewer {

// `selected` is read+write: the panel highlights the currently selected row
// and writes to it whenever a row is clicked. May be null (nothing selected).
// Returns true when any visibility checkbox flipped, so the caller can
// route a Full re-render.
bool drawOutlinePanel(Scene& scene, SceneObject*& selected);

}  // namespace viewer
}  // namespace raygen

#endif
