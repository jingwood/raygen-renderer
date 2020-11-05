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

#define BIN_NAME "raygen"
#define BIN_VER  "1.0.0"

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

void dumpObjects(const Scene& scene, const std::vector<SceneObject*>& objs, string& str);

void dumpMeshes(const std::vector<Mesh*> meshes, string& str) {
	for (const Mesh* mesh : meshes) {
		str.appendFormat("      vertices: %d\n", mesh->vertexCount);
		str.appendFormat("      has normal: %s\n", mesh->hasNormal ? "true" : "false");
		str.appendFormat("      has tangent basis: %s\n", mesh->hasTangentSpaceBasis ? "true" : "false");
	}
}

void dumpObject(const Scene& scene, const SceneObject& obj, string& str) {
	const BoundingBox& bbox = obj.getBoundingBox();
	
	string meshStr;
	dumpMeshes(obj.getMeshes(), meshStr);
	
	const Camera* camera = dynamic_cast<const Camera*>(&obj);
	
	if (camera == scene.mainCamera) {
		str.appendFormat("  %s: (main camera)\n", obj.getName().c_str());
	} else {
		str.appendFormat("  %s:\n", obj.getName().c_str());
	}
	
	str.appendFormat("    location   : (%f, %f, %f)\n"
									 "    angle      : (%f, %f, %f)\n"
									 "    scale      : (%f, %f, %f)\n"
									 "    visible    : %s\n"
									 "    renderable : %s\n"
									 "    bbox       : (%f, %f, %f) ~ (%f, %f, %f)\n",
									 obj.location.z, obj.location.y, obj.location.z,
									 obj.angle.x, obj.angle.y, obj.angle.z,
									 obj.scale.x, obj.scale.y, obj.scale.z,
									 obj.visible ? "true" : "false",
									 obj.renderable ? "true" : "false",
									 bbox.min.x, bbox.min.y, bbox.min.z, bbox.max.x, bbox.max.y, bbox.max.z);
	
	if (camera != NULL) {
		str.appendFormat("    near ~ far : %f ~ %f\n"
										 "    fov        : %f\n"
										 "    dof        : %f\n"
										 "    aperture   : %f\n",
										 camera->viewNear, camera->viewFar,
										 camera->fieldOfView,
										 camera->depthOfField,
										 camera->aperture);
	}

	str.appendFormat("    meshes:\n");
	str.append(meshStr);
	
	dumpObjects(scene, obj.getObjects(), str);
}

void dumpObjects(const Scene& scene, const std::vector<SceneObject*>& objs, string& str) {
	for (const auto* obj : objs) {
		dumpObject(scene, *obj, str);
		str.appendLine();
	}
}

void dumpScene(const Scene& scene, string& str) {
	str.expand(4096);
	
	str.append("scene:\n");
	str.appendLine();
	
	dumpObjects(scene, scene.getObjects(), str);
}

void errorExit(const string& msg) {
	printf(BIN_NAME ": %s", msg.c_str());
	exit(1);
}

void printVerInfo() {
	printf(ANSI_BOLD BIN_NAME " " BIN_VER ANSI_NOR "\n");
}

int main(int argc, const char * argv[]) {

	if (argc < 2) {
		printf("usage: raygen <command> [scene.json|arguments...]\n");
		return 0;
	}

	RendererSettings rs;

	string scenefile, outputImageFile, focusObjectName;
	int inputIndex = 0;
	int tmp_value = 0;

	string cmd;
	bool enableDumpScene = false;
	
	for (int i = 1; i < argc; i++) {
		const char* arg = argv[i];
		
		if (arg[0] == '-') {
			if (IF_ARG("-o")) {
				NEXT_ARG;
				outputImageFile = arg;
			} else if (IF_ARG("-r") || IF_ARG("--resolution")) {
				NEXT_ARG;
				int tmp_res_w = rs.resolutionWidth, tmp_res_h = rs.resolutionHeight;
				int successes = sscanf(arg, "%d,%d", &tmp_res_w, &tmp_res_h);
				if (successes < 2) {
					successes = sscanf(arg, "%dx%d", &tmp_res_w, &tmp_res_h);
				}
				if (successes == 1) {
					rs.resolutionWidth = tmp_res_w;
					rs.resolutionHeight = tmp_res_w;
				} else if (successes == 2) {
					rs.resolutionWidth = tmp_res_w;
					rs.resolutionHeight = tmp_res_h;
				}
			} else if (IF_ARG("--dump")) {
				enableDumpScene = true;
			} else if (IF_ARG("-ver") || IF_ARG("--ver") || IF_ARG("--version")) {
				printVerInfo();
				return 0;
			} else if (IF_ARG("-h") || IF_ARG("--help")) {
				printVerInfo();
				printf("A simple cross-platform ray tracing engine for 3D graphics rendering.\n"
							 "(c) Jingwood, unvell.com, all rights reserved.\n\n");
				printf("usage: ./raygen <cmd> <scene.json> [parameters...]\n"
							 "e.g.   ./raygen render ../../resources/scenes/cubeRoom/cubeRoom.json\n\n");
				printf("  -r | --resolution                    specify resolution of result image\n"
							 "  -s | --samples                       number of ray tracing samples\n"
							 "  -c | --cores | --threads             number of threads/cores to render parallelly\n"
							 "  -ds | --dof-samples                  number of samples on depth of field calculation\n"
							 "  -enaa | --enable-antialias           enable antialias (default: on)\n"
							 "  -encs | --enable-color-sampling      enable read colors from texture (default: on)\n"
							 "  -enpp | --enable-postprocess         eanble post-processes such as grow and blur\n"
							 "  -d | --shader                        specify shader type\n"
							 "  --focus-obj                          make camera look at specified object\n"
							 "  --dump                               dump scene define\n");
				
				return 0;
			}	else READ_ARG_INT("-s", rs.samples)
				else READ_ARG_INT("--samples", rs.samples)
				else READ_ARG_INT("-c", rs.threads)
				else READ_ARG_INT("--threads", rs.threads)
				else READ_ARG_INT("--cores", rs.threads)
				else READ_ARG_INT("-ds", rs.dofSamples)
				else READ_ARG_INT("-dofs", rs.dofSamples)
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
				else READ_ARG_BOL("-cb", rs.cullBackFace)
				else READ_ARG_BOL("--cullback", rs.cullBackFace)
				else if (IF_ARG("-bc") || IF_ARG("--backcolor")) {
					NEXT_ARG;
					color4 c = rs.backColor;
					int successes = sscanf(arg, "%f,%f,%f,%f", &c.r, &c.g, &c.b, &c.a);
					if (successes == 3) {
						c.a = 1;
					}
					rs.backColor = c;
				} else if (IF_ARG("-wc") || IF_ARG("--worldcolor")) {
					NEXT_ARG;
					color4 c = rs.worldColor;
					int successes = sscanf(arg, "%f,%f,%f,%f", &c.r, &c.g, &c.b, &c.a);
					if (successes == 3) {
						c.a = 1;
					}
					rs.worldColor = c;
				}
				else {
					printf("unknown argument: %s\n", arg);
					return 1;
				}
		} else if (i == 1) {
			cmd = arg;
		} else if (inputIndex == 0) {
			scenefile = arg;
			inputIndex++;
		} else {
			printf("unknown argument: %s\n", arg);
			return 1;
		}
	}
	
	if (cmd.isEmpty()) {
		errorExit("no command specified.\n");
	}
	
	if (scenefile.isEmpty()) {
		errorExit("no input file specified.\n");
	}

	File file(scenefile);

	if (outputImageFile.isEmpty()) {
		const string& inpath = file.getPath();
		if (inpath.isEmpty()) {
			outputImageFile.appendFormat("%s.jpg", file.getBaseName().c_str());
		} else {
			outputImageFile.appendFormat("%s%s%s.jpg", file.getPath().c_str(), PATH_SPLITTER_STR, file.getBaseName().c_str());
		}
	}
		
	RayRenderer renderer(&rs);
	RendererSceneLoader loader;
	Scene scene;
	
	loader.load(renderer, &scene, scenefile);
	
	renderer.setScene(&scene);
	renderer.progressCallback = &renderingProgressCallback;
	
	printVerInfo();
	printf("\n");
	printf("  input : %s\n", scenefile.getBuffer());
	printf("  output: %s\n", outputImageFile.getBuffer());
	printf("\n");
	
	printf("  resolution     : %d x %d\n", rs.resolutionWidth, rs.resolutionHeight);
	printf("  cores          : %d\n", rs.threads);
	printf("  shader system  : %s\n", getShaderSystemText(rs.shaderProvider));
	printf("  samples        : %d\n", rs.samples);
	printf("  dof-samples    : %d\n", rs.dofSamples);
	printf("  antialias      : %s\n", rs.enableAntialias ? "yes" : "no");
	printf("  color sampling : %s\n", rs.enableColorSampling ? "yes" : "no");
	printf("  post process   : %s\n", rs.enableRenderingPostProcess ? "yes" : "no");
	printf("  cull backface  : %s\n", rs.cullBackFace ? "yes" : "no");
	printf("  back color     : #%02x%02x%02x%02x\n",
				 (int)(rs.backColor.a * 255), (int)(rs.backColor.r * 255),
				 (int)(rs.backColor.g * 255), (int)(rs.backColor.b * 255));
	printf("  world color    : #ff%02x%02x%02x\n",
				 (int)(rs.worldColor.r * 255), (int)(rs.worldColor.g * 255), (int)(rs.worldColor.b * 255));
	printf("\n");

	if (enableDumpScene) {
		string dumpSceneStr(1024);
		dumpScene(scene, dumpSceneStr);
		std::cout << dumpSceneStr.c_str();
	}
	
	if (scene.mainCamera) {
		Camera& camera = *scene.mainCamera;
		camera.focusOnObjectName = focusObjectName;
	} else {
		std::cout << "warning: main camera not specified\n";
	}

	
	sw.start();

	if (cmd == "render") {
		renderer.render();
	}

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
		printf("(elapsed %s, remaining %s)  \r", _time_str_elapsed.c_str(), _time_str_remaining.c_str());
	} else {
		printf("  \r");
	}
	
	fflush(NULL);
}

#undef IF_ARG
#undef NEXT_ARG
