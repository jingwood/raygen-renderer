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
        // .json (authoring) and .toba (bundle) are both accepted — the
        // loader detects by extension. UTTypeJSON covers .json; .toba
        // doesn't have a registered UTI so we fall back to a custom
        // UTType derived from the extension.
        UTType* tobaType = [UTType typeWithFilenameExtension:@"toba"];
        if (tobaType != nil) {
            panel.allowedContentTypes = @[ UTTypeJSON, tobaType ];
        } else {
            panel.allowedContentTypes = @[ UTTypeJSON ];
        }

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

bool saveBundleFileDialog_mac(char* out, size_t outCap,
                              const char* defaultName, const char* initialDir) {
    if (out == nullptr || outCap == 0) return false;
    out[0] = '\0';

    @autoreleasepool {
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.title = @"Save scene bundle";
        UTType* tobaType = [UTType typeWithFilenameExtension:@"toba"];
        if (tobaType != nil) {
            panel.allowedContentTypes = @[ tobaType ];
        }

        if (defaultName != nullptr && defaultName[0] != '\0') {
            panel.nameFieldStringValue = [NSString stringWithUTF8String:defaultName];
        }
        if (initialDir != nullptr && initialDir[0] != '\0') {
            NSString* dir = [NSString stringWithUTF8String:initialDir];
            panel.directoryURL = [NSURL fileURLWithPath:dir isDirectory:YES];
        }

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
