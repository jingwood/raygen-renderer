///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — file-picker shim implementation.
///////////////////////////////////////////////////////////////////////////////

#include "Dialog.h"

#include <cstring>

#ifdef _WIN32
// commdlg32.lib is in MSVC's default link set, so no project-file change is
// needed to pull in GetOpenFileNameA / GetSaveFileNameA.
#include <windows.h>
#include <commdlg.h>
#endif

namespace raygen {
namespace viewer {

#ifdef __APPLE__
// Implemented in Dialog_mac.mm — NSOpenPanel needs Objective-C++ which we
// don't want to leak into this file (Windows builds don't compile .mm).
bool openSceneFileDialog_mac(char* out, size_t outCap, const char* initialDir);
bool saveBundleFileDialog_mac(char* out, size_t outCap,
                              const char* defaultName, const char* initialDir);
#endif

bool openSceneFileDialog(char* out, size_t outCap, const char* initialDir) {
    if (out == nullptr || outCap == 0) return false;
    out[0] = '\0';

#ifdef _WIN32
    // Filter list is double-NUL-terminated pairs of "label\0pattern\0...\0".
    // The trailing \0 is supplied by the string literal terminator.
    // Both .json (authoring format) and .toba (bundle) are accepted — the
    // loader detects by extension.
    static const char kFilter[] =
        "Scene Files\0*.json;*.toba\0Scene JSON\0*.json\0Scene Bundle\0*.toba\0All Files\0*.*\0";

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
#elif defined(__APPLE__)
    return openSceneFileDialog_mac(out, outCap, initialDir);
#else
    (void)initialDir;
    // TODO: GTK FileChooser / zenity on Linux. Until then the Load Scene
    // button is no-op on Linux; the user can still launch with a scene
    // argv and use Reload.
    return false;
#endif
}

bool saveBundleFileDialog(char* out, size_t outCap,
                          const char* defaultName, const char* initialDir) {
    if (out == nullptr || outCap == 0) return false;
    out[0] = '\0';

#ifdef _WIN32
    // GetSaveFileName uses the same OPENFILENAME struct as the open dialog.
    // Pre-seed `out` with the suggested filename so the user can edit it
    // rather than re-typing.
    if (defaultName != nullptr) {
        size_t n = std::strlen(defaultName);
        if (n >= outCap) n = outCap - 1;
        std::memcpy(out, defaultName, n);
        out[n] = '\0';
    }

    static const char kFilter[] = "Scene Bundle\0*.toba\0All Files\0*.*\0";

    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = nullptr;
    ofn.lpstrFilter     = kFilter;
    ofn.lpstrFile       = out;
    ofn.nMaxFile        = (DWORD)outCap;
    ofn.lpstrInitialDir = initialDir;
    ofn.lpstrTitle      = "Save scene bundle";
    ofn.lpstrDefExt     = "toba";
    // OFN_OVERWRITEPROMPT — Windows asks before clobbering an existing file.
    // OFN_PATHMUSTEXIST keeps the dir validation; OFN_NOCHANGEDIR matches
    // the open path's behaviour.
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetSaveFileNameA(&ofn) != 0;
#elif defined(__APPLE__)
    return saveBundleFileDialog_mac(out, outCap, defaultName, initialDir);
#else
    (void)defaultName; (void)initialDir;
    return false;
#endif
}

}  // namespace viewer
}  // namespace raygen
