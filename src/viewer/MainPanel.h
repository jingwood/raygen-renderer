///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — Main control panel ("raygen viewer" window).
//
//  Top-of-window status / progress / Quality / Camera / Scene / Post-process
//  collapsing sections. Pulled out of main.cpp so the four sub-sections can
//  grow independently from the rest of the viewer.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_main_panel_h__
#define __viewer_main_panel_h__

#include <functional>

#include "ViewerTypes.h"

namespace raygen {
class Camera;
class RayRenderer;
}

namespace raygen {
namespace viewer {

// Bundle of refs and per-frame state the panel needs. Built fresh every frame
// at the call site — no hidden lifetime, the panel never stores it.
struct MainPanelCtx {
    // Read/write — sliders mutate params; pendingDirty is OR'd; cancel button
    // writes renderer.cancelRequested; focus-clear button mutates mainCamera.
    ViewerParams* params;
    ViewerParams* lastKickedParams;
    bool* pendingDirty;
    Camera* mainCamera;          // may be null
    RayRenderer* renderer;

    // Read-only frame state shown in the status header.
    const char* scenePath;
    float fps;
    float lastRenderSec;
    bool isRendering;
    JobKind currentJobKind;
    float previewProgress;

    // Currently attached envmap path (for display in the Scene section).
    // Empty string when no envmap is loaded. May be null.
    const char* envmapPath;

    // Apply a newly picked envmap file to the Scene. Called from the Browse
    // button after the file dialog returns. The caller (main.cpp) loads the
    // texture, swaps Scene::envmap, rebuilds the CDF and kicks a Full
    // render. NULL disables the Browse button.
    std::function<void(const char* path)> onLoadEnvmap;
};

// Returns true when any widget changed a value this frame. Caller wires this
// to its dirty bit to drive the auto-kick at end-of-frame.
bool drawMainPanel(const MainPanelCtx& ctx,
                   const std::function<void(JobKind)>& kickFinal);

}  // namespace viewer
}  // namespace raygen

#endif
