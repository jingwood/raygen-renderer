///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef meshloader_h
#define meshloader_h

#include <stdio.h>
#include "mesh.h"
#include "ucm/stream.h"
#include "ucm/archive.h"

namespace raygen {

#define FORMAT_TAG_MESH 0x6873656d
#define FORMAT_TAG_LMAP 0x70616d6c

enum MeshFileHeaderFlags {
	MHF_HasNormal = 0x2,
	MHF_HasTexcoord = 0x4,
	MHF_HasBoundingBox = 0x8,
	MHF_HasTangentBasisData = 0x10,
	MHF_HasColor = 0x20,
	MHF_HasLightmap = 0x40,
	MHF_HasGrabBoundary = 0x80,
	MHF_HasWireframe = 0x100,
	MHF_HasRefmap = 0x200,
};

enum MeshLightmapTypes {
	MLT_UNKNOWN = 0,
	MLT_JPEG_IMAGE = 1,
	MLT_PNG_IMAGE = 2,
	MLT_RAW_RGB = 3,
	MLT_RADIOSITY = 7,
};

struct MeshFileHeader {
	struct {
		uint formatTag;
		ushort ver;
		ushort flags;
		uint length;
	};
};

struct MeshFileHeader_0100 {
	uint vertexCount;
	uint normalCount;
	uint texcoordCount;
};

struct MeshFileMeta_0101 {
	struct {
		uint vertexCount;
		uint normalCount;
		uint uvCount;
		uint texcoordCount;
		uint indexCount;
	};
	
	BoundingBox bbox;
};

struct MeshFileMeta_0102 {
	struct {
		uint vertexCount;
		uint uvCount;
		uint indexCount;
		uint _reserved1;
		uint _reserved2;
	};
	
	BoundingBox bbox;
};

struct MeshFileMeta_0103 {
	struct {
		uint vertexCount;
		uint uvCount;
		uint indexCount;
		uint _reserved1;
		uint _reserved2;
	};
	
	BoundingBox bbox;
	
	struct {
		uint lightmapTrunkId;
		uint lightmapType;
		uint _reserved4;
		uint _reserved5;
	};
};

struct MeshFileMeta_0104 {
	struct {
		uint vertexCount;
		uint uvCount;
		uint indexCount;
		uint edgeCount;
		uint _reserved2;
	};
	
	BoundingBox bbox;
	
	struct {
		uint lightmapTrunkId;
		uint lightmapType;
		uint refmapTrunkId; // since v0105
		uint _reserved5;
	};
	
	GrabBoundary grabBoundary;
};

typedef MeshFileMeta_0104 MeshFileMeta;

struct MeshLightmapHeader {
	uint formatTag;
	ushort ver;
	ushort type;
	uint resolution;
	uint headerSize;
};

class MeshLoader {
private:
	
public:
	static void load(Mesh& mesh, const string& path);
	static void save(const Mesh& mesh, const char* path);

	static void load(Mesh& mesh, Archive& archive, const uint uid);
	static uint save(const Mesh& mesh, Archive& archive, uint uid = 0);

	static void load(Mesh& mesh, Stream& stream);
	static void save(const Mesh& mesh, Stream& stream);
	
	static void createMeshFileHeader(const Mesh& mesh, MeshFileHeader& header, MeshFileMeta& meta);

};

}

#endif /* meshloader_h */
