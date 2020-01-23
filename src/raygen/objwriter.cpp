///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "objwriter.h"
#include "ucm/file.h"

namespace raygen {

void ObjWriter::writeMesh(const Mesh& mesh, const string& path, const int uvIndex) {
	
	FileStream file(path);
	file.openWrite(FileStreamType::Text);
	
	string line;

	vec3 v1, v2, v3;
	vec3 n1, n2, n3;
	vec2 uv1, uv2, uv3;

	const int triangleCount = mesh.getTriangleCount();
	
	for (int i = 0; i < triangleCount; i++) {
		mesh.getVertex(i, &v1, &v2, &v3);
		line.clear();
		line.appendFormat("v %f %f %f\n", v1.x, v1.y, v1.z);
		line.appendFormat("v %f %f %f\n", v2.x, v2.y, v2.z);
		line.appendFormat("v %f %f %f\n", v3.x, v3.y, v3.z);
		file.writeText(line);
	}

	for (int i = 0; i < triangleCount; i++) {
		mesh.getUV(uvIndex, i, &uv1, &uv2, &uv3);
		line.clear();
		line.appendFormat("vt %f %f\n", uv1.x, uv1.y);
		line.appendFormat("vt %f %f\n", uv2.x, uv2.y);
		line.appendFormat("vt %f %f\n", uv3.x, uv3.y);
		file.writeText(line);
	}

	for (int i = 0; i < triangleCount; i++) {
		mesh.getNormal(i, &n1, &n2, &n3);
		line.clear();
		line.appendFormat("vn %f %f %f\n", n1.x, n1.y, n1.z);
		line.appendFormat("vn %f %f %f\n", n2.x, n2.y, n2.z);
		line.appendFormat("vn %f %f %f\n", n3.x, n3.y, n3.z);
		file.writeText(line);
	}

	for (int i = 0; i < triangleCount; i++) {
		line.clear();
		const int i1 = i * 3 + 1;
		const int i2 = i * 3 + 2;
		const int i3 = i * 3 + 3;
		line.appendFormat("f %d/%d/%d %d/%d/%d %d/%d/%d\n", i1, i1, i1, i2, i2, i2, i3, i3, i3);
		file.writeText(line);
	}

	file.close();
}

}