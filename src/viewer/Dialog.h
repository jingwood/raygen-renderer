///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — native file-picker shim.
//
//  ImGui has no built-in file dialog. Rather than pull a third-party widget
//  into the build we delegate to the OS's standard Open dialog (GetOpenFileName
//  on Windows; macOS / Linux currently stubs out and returns false until a
//  Cocoa / GTK implementation lands).
///////////////////////////////////////////////////////////////////////////////

#ifndef __viewer_dialog_h__
#define __viewer_dialog_h__

#include <cstddef>

namespace raygen {
namespace viewer {

// Opens an "Open scene" dialog filtered to .json / .toba. Returns true on
// selection and writes a NUL-terminated path into `out` (truncated to
// `outCap-1` bytes). Returns false if the user cancelled, on platforms with
// no implementation, or if `out` is null.
//
// `initialDir` (may be null) seeds the dialog's working directory; we don't
// change the process CWD even if the user navigates elsewhere.
bool openSceneFileDialog(char* out, size_t outCap, const char* initialDir);

// "Save bundle as" dialog. Returns true on Save, writing the chosen path to
// `out`. The dialog defaults to a `.toba` extension and seeds the suggested
// filename from `defaultName` (just the basename, no path). Behaves like
// openSceneFileDialog otherwise — false on cancel / unsupported platform.
bool saveBundleFileDialog(char* out, size_t outCap,
                          const char* defaultName, const char* initialDir);

// Generic "Open image" dialog for picking texture / envmap files. Filters
// the common image formats supported by the renderer (.jpg/.png/.bmp/.hdr).
// `title` may be null (defaults to "Open image"); `initialDir` may be null.
// Same return contract as the other dialogs.
bool openImageFileDialog(char* out, size_t outCap,
                         const char* title, const char* initialDir);

}  // namespace viewer
}  // namespace raygen

#endif
