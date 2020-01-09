///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include <iostream>
#include "raygen/rayrenderer.h"
#include "raygen/sceneloader.h"
#include "ugm/imgcodec.h"
#include "ucm/stopwatch.h"
#include "ucm/ansi.h"
#include "ucm/strutil.h"

using namespace ucm;
using namespace ugm;
using namespace raygen;

#define IF_ARG(ARG) (strcmp(arg, ARG) == 0)
#define NEXT_ARG if (++i >= argc) break; arg = argv[i]

#define READ_ARG_INT(PARAM, VAR)		 if (IF_ARG(PARAM)) { \
		NEXT_ARG; \
		if (sscanf(arg, "%d", &tmp_value) == 1) { \
			VAR = tmp_value; \
		} \
	}

#define READ_ARG_STR(PARAM, VAR)		if (IF_ARG(PARAM)) { \
		NEXT_ARG; \
		VAR = arg; \
	}

#define STR_CMP_CASE_INSEN(a, b) string::compare(a, b, StringComparingFlags::SCF_CASE_INSENSITIVE)
#define IS_ARG_CASE_INSEN(b) STR_CMP_CASE_INSEN(arg, b)

#define READ_ARG_BOL(PARAM, VAR)		 if (IF_ARG(PARAM)) { \
		NEXT_ARG; \
		VAR = IS_ARG_CASE_INSEN("yes") || IS_ARG_CASE_INSEN("true") \
			 || IS_ARG_CASE_INSEN("1")   || IS_ARG_CASE_INSEN("on");  \
	}

void renderingProgressCallback(const float progressRate);

const char* getShaderSystemText(const byte v) {
	switch (v) {
		case 0: return "Simple";
		case 1: return "Ambient Occlusion";
		case 2: return "Lambert";
		case 3: return "Lambert with AO";
		case 5: return "BSDF";
		default: return "Unknown";
	}
}

static Stopwatch sw;

int main(int argc, const char * argv[]) {

	if (argc < 2) {
		printf("usage: raygen <command> [scene.json|arguments...]\n");
		return 0;
	}

	RendererSettings rs;

	string scenefile, outputImageFile, focusObjectName;
	int inputIndex = 0;
	int tmp_value = 0;

	string cmd = argv[1];
	
	for (int i = 2; i < argc; i++) {
		const char* arg = argv[i];
		
		if (arg[0] == '-') {
			if (IF_ARG("-o")) {
				NEXT_ARG;
				outputImageFile = arg;
			} else if (IF_ARG("-r")) {
				NEXT_ARG;
				int tmp_res_w = rs.resolutionWidth, tmp_res_h = rs.resolutionHeight;
				int successes = sscanf(arg, "%d,%d", &tmp_res_w, &tmp_res_h);
				if (successes == 1) {
					rs.resolutionWidth = tmp_res_w;
					rs.resolutionHeight = tmp_res_w;
				} else if (successes == 2) {
					rs.resolutionWidth = tmp_res_w;
					rs.resolutionHeight = tmp_res_h;
				}
			} else READ_ARG_INT("-s", rs.samples)
				else READ_ARG_INT("--samples", rs.samples)
				else READ_ARG_INT("-c", rs.threads)
				else READ_ARG_INT("--cores", rs.threads)
				else READ_ARG_INT("-ds", rs.dofSamples)
				else READ_ARG_INT("--dof-samples", rs.dofSamples)
				else READ_ARG_BOL("-enaa", rs.enableAntialias)
				else READ_ARG_BOL("--enable-antialias", rs.enableAntialias)
				else READ_ARG_BOL("-encs", rs.enableColorSampling)
				else READ_ARG_BOL("--enable-color-sampling", rs.enableColorSampling)
				else READ_ARG_BOL("-enpp", rs.enableRenderingPostProcess)
				else READ_ARG_BOL("--enable-postprocess", rs.enableRenderingPostProcess)
				else READ_ARG_INT("-d", rs.shaderProvider)
				else READ_ARG_INT("--shader", rs.shaderProvider)
				else READ_ARG_STR("--focus-obj", focusObjectName)

		} else if (inputIndex == 0) {
			scenefile = arg;
		}
	}

	File file(scenefile);

	if (outputImageFile.isEmpty()) {
		const string& inpath = file.getPath();
		if (inpath.isEmpty()) {
			outputImageFile.appendFormat("%s.jpg", file.getBaseName().c_str());
		} else {
			outputImageFile.appendFormat("%s/%s.jpg", file.getPath().c_str(), file.getBaseName().c_str());
		}
	}
	
	printf("input : %s\n", scenefile.getBuffer());
	printf("output: %s\n", outputImageFile.getBuffer());
	printf("\n");
	printf("resolution     : %d x %d\n", rs.resolutionWidth, rs.resolutionHeight);
	printf("cores          : %d\n", rs.threads);
	printf("shader system  : %s\n", getShaderSystemText(rs.shaderProvider));
	printf("samples        : %d\n", rs.samples);
	printf("dof-samples    : %d\n", rs.dofSamples);
	printf("antialias      : %s\n", rs.enableAntialias ? "yes" : "no");
	printf("color sampling : %s\n", rs.enableColorSampling ? "yes" : "no");
	printf("post process   : %s\n", rs.enableRenderingPostProcess ? "yes" : "no");
	printf("\n");
	
	RayRenderer renderer(&rs);
	RendererSceneLoader loader;
	Scene scene;
	
	loader.load(renderer, &scene, scenefile);
	
	renderer.setScene(&scene);
	renderer.progressCallback = &renderingProgressCallback;
	
	if (scene.mainCamera) {
		scene.mainCamera->focusOnObjectName = focusObjectName;
	}
	
	sw.start();
	renderer.render();
	sw.stop();
	
	const Image& renderImage = renderer.getRenderResult();	
	saveImage(renderImage, outputImageFile);
	
	static string _time_str_done;
	formatFriendlyDate(sw.getElapsedSeconds(), _time_str_done);
	printf(ANSI_RESET_LINE "done. (%s)\n", _time_str_done.c_str());
}

void renderingProgressCallback(const float progressRate) {
	printf(ANSI_RESET_LINE "rendering... %d%% ", int(progressRate * 100.0f));
	
	const double elapsedTime = sw.getElapsedSeconds();
	if (elapsedTime > 3) {
		static string _time_str_elapsed, _time_str_remaining;
		formatFriendlyDate(elapsedTime, _time_str_elapsed);
		formatFriendlyDate((1.0 - progressRate) * elapsedTime / progressRate, _time_str_remaining);
		printf("(elapsed %s, remaining %s)", _time_str_elapsed.c_str(), _time_str_remaining.c_str());
	}
	
	fflush(NULL);
}

#undef IF_ARG
#undef NEXT_ARG
