///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — macOS NSOpenPanel implementation of the file-picker shim.
//
//  Compiled as Objective-C++ (.mm). Dispatched to from Dialog.cpp under the
//  __APPLE__ branch so the rest of the viewer stays plain C++.
///////////////////////////////////////////////////////////////////////////////

#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cstddef>
#include <cstring>

namespace raygen {
namespace viewer {

bool openSceneFileDialog_mac(char* out, size_t outCap, const char* initialDir) {
    if (out == nullptr || outCap == 0) return false;
    out[0] = '\0';

    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.title = @"Open scene";
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        // UTTypeJSON covers the .json extension; allowing the generic JSON
        // UTI lets the user pick scenes that may have non-.json mime
        // associations from other apps too.
        panel.allowedContentTypes = @[ UTTypeJSON ];

        if (initialDir != nullptr && initialDir[0] != '\0') {
            NSString* dir = [NSString stringWithUTF8String:initialDir];
            panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
        }

        // runModal blocks until the user dismisses the panel. The viewer
        // worker thread keeps rendering — only the GLFW main loop pauses,
        // which is fine since the user is interacting with the dialog.
        if ([panel runModal] != NSModalResponseOK) return false;

        NSURL* url = panel.URL;
        if (url == nil) return false;

        const char* path = url.fileSystemRepresentation;
        if (path == nullptr) return false;

        std::strncpy(out, path, outCap - 1);
        out[outCap - 1] = '\0';
        return true;
    }
}

}  // namespace viewer
}  // namespace raygen
