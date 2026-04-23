///////////////////////////////////////////////////////////////////////////////
//  Raygen Viewer
//  Cross-platform scene viewer + tuner for the raygen renderer.
//
//  Step 3: loads a scene.json, runs raygen synchronously on demand, uploads
//  the result to a GL texture and displays it in an ImGui window. Step 4 will
//  introduce sliders + background re-render; Step 5 the progressive preview.
//
//  MIT License
///////////////////////////////////////////////////////////////////////////////

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <GLFW/glfw3.h>

// Windows ships with only GL 1.1 constants in <GL/gl.h>. Declare the handful of
// 1.2+ enums we use so we don't have to pull in GLEW/GLAD just for these.
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 0x8058
#endif

#include "raygen/rayrenderer.h"
#include "raygen/sceneloader.h"
#include "ugm/image.h"

using namespace raygen;
using namespace ugm;

static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Repack a raygen Image (arbitrary internal pixel format) into a tightly packed
// RGBA8 buffer for glTexImage2D. Using per-pixel getPixel keeps us independent
// of whatever format the renderer happens to use; speed is not a concern here
// because this only runs once per render.
static void uploadImageToTexture(const Image& img, GLuint& tex, int& w, int& h) {
    w = (int)img.width();
    h = (int)img.height();
    if (w <= 0 || h <= 0) return;

    std::vector<unsigned char> rgba(w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            color4f c = img.getPixel(x, y);
            auto clamp01 = [](float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
            const size_t i = (size_t)(y * w + x) * 4;
            rgba[i + 0] = (unsigned char)(clamp01(c.r) * 255.0f);
            rgba[i + 1] = (unsigned char)(clamp01(c.g) * 255.0f);
            rgba[i + 2] = (unsigned char)(clamp01(c.b) * 255.0f);
            rgba[i + 3] = 255;
        }
    }

    if (tex == 0) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
}

int main(int argc, char** argv) {
    const char* scenePath = (argc > 1)
        ? argv[1]
        : "F:\\3D Models\\F2\\raygen_export\\F2.json";

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) return 1;

    const char* glsl_version = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "raygen viewer", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    RendererSettings rs;
    rs.resolutionWidth = 600;
    rs.resolutionHeight = 375;
    rs.samples = 4;
    rs.enableDenoise = true;

    RayRenderer renderer(&rs);
    Scene scene;
    RendererSceneLoader loader;
    loader.load(renderer, &scene, scenePath);
    renderer.setScene(&scene);

    GLuint renderTex = 0;
    int texW = 0, texH = 0;
    bool needRender = true;
    double lastRenderSec = 0.0;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        if (needRender) {
            double t0 = glfwGetTime();
            renderer.render();
            lastRenderSec = glfwGetTime() - t0;
            uploadImageToTexture(renderer.getRenderResult(), renderTex, texW, texH);
            needRender = false;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("raygen viewer");
        ImGui::Text("scene: %s", scenePath);
        ImGui::Text("resolution: %d x %d   samples: %d", rs.resolutionWidth, rs.resolutionHeight, rs.samples);
        ImGui::Text("last render: %.2f s", lastRenderSec);
        ImGui::Separator();
        if (ImGui::Button("Re-render")) needRender = true;
        ImGui::SameLine();
        ImGui::Text("FPS: %.1f", io.Framerate);
        ImGui::End();

        ImGui::Begin("render", nullptr, ImGuiWindowFlags_HorizontalScrollbar);
        if (renderTex != 0) {
            ImGui::Image((ImTextureID)(intptr_t)renderTex, ImVec2((float)texW, (float)texH));
        } else {
            ImGui::Text("(no render yet)");
        }
        ImGui::End();

        ImGui::Render();
        int fbW, fbH;
        glfwGetFramebufferSize(window, &fbW, &fbH);
        glViewport(0, 0, fbW, fbH);
        glClearColor(0.10f, 0.11f, 0.13f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    if (renderTex) glDeleteTextures(1, &renderTex);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
