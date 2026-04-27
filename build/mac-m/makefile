CXX=clang++
AR=ar
CXXFLAGS=-O3 -ffast-math -funroll-loops -flto -mcpu=apple-m1 -std=c++14 -pthread -Wall -arch arm64 #-pedantic
PD=../../      # Project Directory
LIB=../../lib/mac-m/libjpeg.a ../../lib/mac-m/libpng.a ../../lib/mac-m/libz.a \
    ../../lib/mac-m/libucm.a ../../lib/mac-m/libugm.a
CX=$(CXX) $(CXXFLAGS)
BIN=raygen
DEV_BIN=libraygen.a

# Change this to the folder (cpp-common-class, libucm)
INC+=-I../../../cpp-common-class/src -I../../../cpp-graphics-math/src

VPATH=../../src/raygen

SRCS = $(wildcard $(VPATH)/*.cpp)
OBJS = $(patsubst %.cpp, %.o, $(notdir $(SRCS)))

all: $(DEV_BIN) $(BIN)

$(DEV_BIN): $(OBJS)
	$(AR) -crv $(DEV_BIN) $(OBJS)

$(BIN): $(DEV_BIN) main.o
	$(CX) $(LIB) -o $(BIN) main.o $(DEV_BIN) -flto
	@echo "Build successed."
	@echo "Library file generated: $(DEV_BIN)"
	@echo "Executable binary file generated: $(BIN)"

main.o: ../../src/main.cpp
	$(CX) $(INC) -c ../../src/main.cpp -o main.o

%.o:	%.cpp
	$(CX) $(INC) -c $< -o $@

# --- Viewer (opt-in: `make viewer`) -----------------------------------------
# Interactive tuner built on Dear ImGui + GLFW + OpenGL. Requires GLFW
# (install via `brew install glfw`). Override GLFW_PREFIX if brew isn't at the
# default Apple Silicon location.
GLFW_PREFIX ?= /opt/homebrew/opt/glfw
IMGUI_DIR=../../inc/imgui
VIEWER_BIN=raygen-viewer
VIEWER_SRC=../../src/viewer/main.cpp
VIEWER_INC=-I../../src -I../../src/viewer -I../../../cpp-common-class/src -I../../../cpp-graphics-math/src \
           -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends -I$(GLFW_PREFIX)/include
# macOS deprecated the OpenGL framework in 10.14 but still ships it; silence
# the ~300 deprecation warnings from viewer and imgui_impl_opengl3 sources.
VIEWER_DEFS=-DGL_SILENCE_DEPRECATION
VIEWER_LIB=-L$(GLFW_PREFIX)/lib -lglfw \
           -framework Cocoa -framework OpenGL -framework IOKit -framework CoreVideo -framework CoreFoundation

IMGUI_CORE_OBJS=imgui.o imgui_draw.o imgui_tables.o imgui_widgets.o imgui_demo.o
IMGUI_BACKEND_OBJS=imgui_impl_glfw.o imgui_impl_opengl3.o
VIEWER_PANEL_OBJS=viewer_main_panel.o viewer_medium_editor.o viewer_property_panel.o viewer_types.o
VIEWER_OBJS=viewer_main.o $(VIEWER_PANEL_OBJS) $(IMGUI_CORE_OBJS) $(IMGUI_BACKEND_OBJS)

viewer: $(VIEWER_BIN)

$(VIEWER_BIN): $(DEV_BIN) $(VIEWER_OBJS)
	$(CX) -o $(VIEWER_BIN) $(VIEWER_OBJS) $(DEV_BIN) $(LIB) $(VIEWER_LIB) -flto
	@echo "Viewer binary generated: $(VIEWER_BIN)"

viewer_main.o: $(VIEWER_SRC)
	$(CX) $(VIEWER_DEFS) $(VIEWER_INC) -c $(VIEWER_SRC) -o viewer_main.o

viewer_main_panel.o: ../../src/viewer/MainPanel.cpp ../../src/viewer/MainPanel.h ../../src/viewer/ViewerTypes.h
	$(CX) $(VIEWER_DEFS) $(VIEWER_INC) -c ../../src/viewer/MainPanel.cpp -o viewer_main_panel.o

viewer_medium_editor.o: ../../src/viewer/MediumEditor.cpp ../../src/viewer/MediumEditor.h
	$(CX) $(VIEWER_DEFS) $(VIEWER_INC) -c ../../src/viewer/MediumEditor.cpp -o viewer_medium_editor.o

viewer_property_panel.o: ../../src/viewer/PropertyPanel.cpp ../../src/viewer/PropertyPanel.h ../../src/viewer/MediumEditor.h
	$(CX) $(VIEWER_DEFS) $(VIEWER_INC) -c ../../src/viewer/PropertyPanel.cpp -o viewer_property_panel.o

viewer_types.o: ../../src/viewer/ViewerTypes.cpp ../../src/viewer/ViewerTypes.h
	$(CX) $(VIEWER_DEFS) $(VIEWER_INC) -c ../../src/viewer/ViewerTypes.cpp -o viewer_types.o

$(IMGUI_CORE_OBJS): %.o: $(IMGUI_DIR)/%.cpp
	$(CX) -I$(IMGUI_DIR) -c $< -o $@

$(IMGUI_BACKEND_OBJS): %.o: $(IMGUI_DIR)/backends/%.cpp
	$(CX) $(VIEWER_DEFS) -I$(IMGUI_DIR) -I$(GLFW_PREFIX)/include -c $< -o $@

clean:
	rm -f $(BIN) $(DEV_BIN) $(VIEWER_BIN) *.o
	rm -rf $(BIN).dSYM $(VIEWER_BIN).dSYM
