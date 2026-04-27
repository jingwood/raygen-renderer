///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer — File panel implementation.
///////////////////////////////////////////////////////////////////////////////

#include "FilePanel.h"

#include <cstring>

#include "imgui.h"

#include "raygen/rayrenderer.h"
#include "ucm/string.h"
#include "ugm/imgcodec.h"

#include "Dialog.h"

namespace raygen {
namespace viewer {

namespace {

// Compute the directory portion of `scenePath` into `outDir`. Used to seed
// the Load Scene dialog so it opens next to the current scene rather than at
// the OS default.
void deriveSceneDir(const char* scenePath, char* outDir, size_t outDirCap) {
    outDir[0] = '\0';
    if (scenePath == nullptr || outDirCap == 0) return;

    const char* lsU = std::strrchr(scenePath, '/');
    const char* lsW = std::strrchr(scenePath, '\\');
    const char* ls  = lsU > lsW ? lsU : lsW;
    if (ls == nullptr) return;

    size_t n = (size_t)(ls - scenePath);
    if (n >= outDirCap) n = outDirCap - 1;
    std::memcpy(outDir, scenePath, n);
    outDir[n] = '\0';
}

}  // namespace

void drawFilePanel(const FilePanelCtx& ctx) {
    ImGui::Begin("File");

    const bool hasScene = (ctx.scenePath != nullptr && ctx.scenePath[0] != '\0');
    if (hasScene) {
        ImGui::TextWrapped("scene: %s", ctx.scenePath);
    } else {
        ImGui::TextDisabled("scene: (none — use Load Scene...)");
    }

    // Reload + Load both touch the Scene unique_ptr, which the worker thread
    // captures by reference at the start of each job — only safe to swap
    // while the worker is idle.
    const bool canFile = !ctx.isRendering;
    if (!canFile) ImGui::BeginDisabled();

    if (ImGui::Button("Load scene...")) {
        char initDir[512];
        deriveSceneDir(ctx.scenePath, initDir, sizeof(initDir));

        char picked[1024] = {0};
        const char* seed = initDir[0] ? initDir : nullptr;
        if (openSceneFileDialog(picked, sizeof(picked), seed)) {
            ctx.onLoadScene(picked);
        }
    }
    ImGui::SameLine();
    // Reload only makes sense when we have a current scenePath; greying it
    // out also matches user expectation ("nothing to reload yet").
    if (!hasScene) ImGui::BeginDisabled();
    if (ImGui::Button("Reload scene")) {
        ctx.onReloadScene();
    }
    if (!hasScene) ImGui::EndDisabled();

    if (!canFile) ImGui::EndDisabled();

    ImGui::InputText("output path", ctx.outputPath, ctx.outputPathCap);

    const bool canSave = !ctx.isRendering && ctx.hasRender && ctx.outputPath[0] != 0;
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Save render")) {
        // .hdr → linear-radiance HDR buffer (float, no tonemap).
        // Anything else → tonemapped LDR preview. saveImage routes by
        // extension via getImageFormatByExtension.
        ucm::string outPath(ctx.outputPath);
        ugm::ImageCodecFormat outFmt = ugm::ImageCodecFormat::ICF_AUTO;
        ugm::getImageFormatByExtension(outPath, &outFmt);
        if (outFmt == ugm::ImageCodecFormat::ICF_HDR) {
            ugm::saveImage(ctx.renderer->getHdrResult(), outPath);
        } else {
            ugm::saveImage(ctx.renderer->getRenderResult(), outPath);
        }
    }
    if (!canSave) ImGui::EndDisabled();

    ImGui::End();
}

}  // namespace viewer
}  // namespace raygen
