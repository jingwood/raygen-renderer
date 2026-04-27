///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — file-picker shim implementation.
///////////////////////////////////////////////////////////////////////////////

#include "Dialog.h"

#ifdef _WIN32
// commdlg32.lib is in MSVC's default link set, so no project-file change is
// needed to pull in GetOpenFileNameA.
#include <windows.h>
#include <commdlg.h>
#endif

namespace raygen {
namespace viewer {

bool openSceneFileDialog(char* out, size_t outCap, const char* initialDir) {
    if (out == nullptr || outCap == 0) return false;
    out[0] = '\0';

#ifdef _WIN32
    // Filter list is double-NUL-terminated pairs of "label\0pattern\0...\0".
    // The trailing \0 is supplied by the string literal terminator.
    static const char kFilter[] = "Scene JSON\0*.json\0All Files\0*.*\0";

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = nullptr;  // non-modal to GLFW window — acceptable
    ofn.lpstrFilter     = kFilter;
    ofn.lpstrFile       = out;
    ofn.nMaxFile        = (DWORD)outCap;
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrTitle      = "Open scene";
    // OFN_NOCHANGEDIR keeps the process CWD intact even if the user navigates
    // away during the dialog. Texture loading resolves relative paths from
    // the scene file's directory, but other parts of the codebase may rely
    // on CWD staying put.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameA(&ofn) != 0;
#else
    (void)initialDir;
    // TODO: NSOpenPanel on macOS, GTK FileChooser / zenity on Linux. Until
    // then the Load Scene button is no-op outside Windows; the user can
    // still launch with a scene argv and use Reload.
    return false;
#endif
}

}  // namespace viewer
}  // namespace raygen
