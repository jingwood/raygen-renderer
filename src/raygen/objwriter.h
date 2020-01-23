///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef objwriter_h
#define objwriter_h

#include <stdio.h>

#include "ucm/string.h"
#include "mesh.h"

namespace raygen {

class ObjWriter {
	
public:
	bool enableDataReuse = true;
	
	static void writeMesh(const Mesh& mesh, const string& path, const int uvIndex = 0);
};

}

#endif /* objwriter_h */