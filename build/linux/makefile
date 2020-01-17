CXX=clang++
AR=ar
CXXFLAGS=-O3 -std=c++11 -pthread -Wall #-pedantic
#INC=-I../../inc/jpeg-6b -I../../inc/
LIB=-ljpeg -lpng -lz
LIB_DEV=../../lib/linux/libucm.a ../../lib/linux/libugm.a
CX=$(CXX) $(CXXFLAGS)
BIN=raygen
DEV_BIN=libraygen.a

# Change this to the folder (cpp-common-class, libucm)
INC+=-I../../../cpp-common-class/src -I../../../cpp-graphics-module/src

VPATH=../../src/raygen

SRCS = $(wildcard $(VPATH)/*.cpp)
OBJS = $(patsubst %.cpp, %.o, $(notdir $(SRCS)))

all: $(DEV_BIN) $(BIN)

$(DEV_BIN): $(OBJS)
	$(AR) -crv $(DEV_BIN) $(OBJS)

$(BIN): $(DEV_BIN) main.o
	$(CX) $(LIB) -o $(BIN) main.o $(DEV_BIN) $(LIB_DEV)

main.o: ../../src/main.cpp
	$(CX) $(INC) -c ../../src/main.cpp -o main.o

%.o:    %.cpp
	$(CX) $(INC) -c $< -o $@

clean:
	rm -f $(BIN) $(DEV_BIN) *.o
	rm -rf $(BIN).dSYM

