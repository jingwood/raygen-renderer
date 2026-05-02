///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — File panel (load/reload scene + save render).
//
//  Layouts the "File" window: Load scene, Reload scene, output path input,
//  and Save render. The panel itself doesn't do scene swaps — those touch
//  too many root-scope objects (Scene unique_ptr, sidecar, default output
//  path, kick) so the caller hands in callbacks for the heavy lifting and
//  the panel just decides when to fire them.
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_file_panel_h__
#define __viewer_file_panel_h__

#include <cstddef>
#include <functional>

namespace raygen { class RayRenderer; }

namespace raygen {
namespace viewer {

struct FilePanelCtx {
    // Display + dialog seed (read-only). Always non-null.
    const char* scenePath;

    // Mutable buffer backing the "output path" InputText. Caller owns it; we
    // mutate in place so Save uses whatever the user typed.
    char* outputPath;
    size_t outputPathCap;

    // Frame state.
    bool isRendering;            // disables Reload / Load while a job runs
    bool hasRender;              // true once any job has finished — gates Save

    // Renderer ref, used by Save to pick LDR vs HDR buffer.
    const RayRenderer* renderer;

    // Scene swap callbacks. The panel never touches Scene state itself; it
    // just signals intent and lets main.cpp do the unique_ptr swap, sidecar
    // overlay, kick, etc.
    //   onReloadScene    — Reload button: re-parse the *current* scene path.
    //   onLoadScene(p)   — Load Scene dialog returned `p`: caller should
    //                      update its scenePath buffer, recompute sidecar /
    //                      default output path, then run the same load flow.
    //   onSaveViewer     — Save viewer button: flush current ViewerParams
    //                      (sliders, env, post-process, output) to the
    //                      sidecar. Lets users commit a known-good state
    //                      without waiting for the next render kick.
    std::function<void()>                    onReloadScene;
    std::function<void(const char* newPath)> onLoadScene;
    std::function<void()>                    onSaveViewer;

    // Save current scene tree as a self-contained .toba bundle. Caller
    // shows the save dialog, writes the archive (manifest + thumbnail +
    // mesh / texture chunks), and may then update its scenePath buffer to
    // the saved location so subsequent Reload targets the bundle. NULL
    // disables the Save Bundle button.
    std::function<void()>                    onSaveBundle;
};

void drawFilePanel(const FilePanelCtx& ctx);

}  // namespace viewer
}  // namespace raygen

#endif
