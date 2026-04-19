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

%.o:    %.cpp
	$(CX) $(INC) -c $< -o $@

clean:
	rm -f $(BIN) $(DEV_BIN) *.o
	rm -rf $(BIN).dSYM

