///////////////////////////////////////////////////////////////////////////////
//  Raygen Renderer
//  A simple cross-platform ray tracing engine for 3D graphics rendering.
//
//  MIT License
//  (c) 2016-2020 Jingwood, unvell.com, all rights reserved.
///////////////////////////////////////////////////////////////////////////////

#include "meshloader.h"

namespace raygen {

#define CURRENT_MESH_VER 0x0105

void MeshLoader::load(Mesh& mesh, const string& path) {
	FileStream stream(path);
	stream.openRead();

	MeshLoader::load(mesh, stream);
	
	stream.close();
}

void MeshLoader::load(Mesh& mesh, Stream& stream) {
	
	const size_t startpos = stream.getPosition();
	
	MeshFileHeader header;
	stream.read(&header, sizeof(MeshFileHeader));

	MeshFileMeta meta;

	if (header.formatTag == FORMAT_TAG_MESH) {
		if (header.ver < 0x0102) {
			MeshFileMeta_0101 meta0101;
			stream.read(&meta0101, sizeof(meta0101));
			
			meta.vertexCount = meta0101.vertexCount;
			meta.uvCount = meta0101.uvCount;
			meta.indexCount = meta0101.indexCount;
			
			mesh.hasNormal = meta0101.normalCount > 0;
			mesh.hasTexcoord = meta0101.texcoordCount > 0;
			mesh.hasTangentSpaceBasis = (header.flags & 0x2 /* MHF_HasTangentBasisData */) == 0x2;
			
		}	else {
			
			if (header.ver < 0x0103) {
				MeshFileMeta_0102 meta0102;
				stream.read(&meta0102, sizeof(meta0102));
				
				memcpy(&meta, &meta0102, sizeof(meta0102));
			} else {
				stream.read(&meta, sizeof(meta));
			}

			mesh.hasNormal = (header.flags & MeshFileHeaderFlags::MHF_HasNormal) == MeshFileHeaderFlags::MHF_HasNormal;
			mesh.hasTexcoord = (header.flags & MeshFileHeaderFlags::MHF_HasTexcoord) == MeshFileHeaderFlags::MHF_HasTexcoord;
			mesh.hasTangentSpaceBasis = (header.flags & MeshFileHeaderFlags::MHF_HasTangentBasisData) == MeshFileHeaderFlags::MHF_HasTangentBasisData;
			mesh.hasBoundingBox = (header.flags & MeshFileHeaderFlags::MHF_HasBoundingBox) == MeshFileHeaderFlags::MHF_HasBoundingBox;
			
			if (header.ver >= 0x0103) {
				mesh.hasColor = (header.flags & MeshFileHeaderFlags::MHF_HasColor) == MeshFileHeaderFlags::MHF_HasColor;
				
				if (header.ver >= 0x0104) {
					mesh.hasGrabBoundary = (header.flags & MeshFileHeaderFlags::MHF_HasGrabBoundary) == MeshFileHeaderFlags::MHF_HasGrabBoundary;
					mesh.grabBoundary = meta.grabBoundary;
					
					if ((header.flags & MeshFileHeaderFlags::MHF_HasWireframe) && meta.edgeCount > 0) {
						mesh.edgeCount = meta.edgeCount;
					}
				}
			}
		}
		
		stream.setPosition(header.length);
	}
	else {
		stream.setPosition(startpos);
		
		MeshFileHeader_0100 oldHeader;
		stream.read(&oldHeader, sizeof(oldHeader));
		
		meta.vertexCount = oldHeader.vertexCount;
		meta.uvCount = 1;
		meta.indexCount = 0;
	}
	
	if (!mesh.hasTexcoord) {
		meta.uvCount = 0;
	}
	
	mesh.init(meta.vertexCount, meta.uvCount, meta.indexCount);
	
	if (mesh.vertexCount > 0) {
		stream.read(mesh.vertices, sizeof(vec3) * meta.vertexCount);
	}
	
	if (mesh.hasNormal) {
		stream.read(mesh.normals, sizeof(vec3) * meta.vertexCount);
	}
	
	if (mesh.hasTexcoord && mesh.uvCount > 0) {
		stream.read(mesh.texcoords, sizeof(vec2) * meta.vertexCount * meta.uvCount);
	}
	
	if (mesh.hasTangentSpaceBasis) {
		stream.read(mesh.tangents, sizeof(vec3) * meta.vertexCount);
		stream.read(mesh.bitangents, sizeof(vec3) * meta.vertexCount);
	}
	
	if (mesh.hasColor) {
		stream.read(mesh.colors, sizeof(color3) * meta.vertexCount);
	}
	
	if (mesh.indexCount > 0) {
		stream.read(mesh.indexes, sizeof(vertex_index_t) * meta.indexCount);
	}
	
	if (mesh.hasBoundingBox) {
		mesh.bbox = meta.bbox;
	}
	
	if (mesh.hasLightmap) {
		// TODO
	}
	
	if (mesh.hasRefmap) {
		// TODO
	}
	
	if (mesh.edgeCount > 0) {
		mesh.edges = new Edge[meta.edgeCount];
		stream.read(mesh.edges, meta.edgeCount * sizeof(Edge));
	}
}

void MeshLoader::save(const Mesh& mesh, const char* path) {
	FileStream stream(path);
	stream.openWrite();
	
	MeshLoader::save(mesh, stream);
	
	stream.close();
}

void MeshLoader::load(Mesh& mesh, Archive& archive, const uint uid) {
	ChunkEntry* entry = archive.openChunk(uid, FORMAT_TAG_MESH);
	if (entry != NULL) {
		MeshLoader::load(mesh, *entry->stream);
		archive.closeChunk(entry);
	}
}

uint MeshLoader::save(const Mesh &mesh, Archive &archive, uint uid) {
	ChunkEntry* entry = NULL;
	
	if (uid == 0) {
		entry = archive.newChunk(FORMAT_TAG_MESH);
		uid = entry->uid;
	} else {
		entry = archive.openChunk(uid, FORMAT_TAG_MESH);
	}
	
	MeshLoader::save(mesh, *entry->stream);
	archive.updateAndCloseChunk(entry);
	
	return uid;
}

void MeshLoader::save(const Mesh& mesh, Stream& stream) {
	
	MeshFileHeader header;
	header.formatTag = FORMAT_TAG_MESH;
	header.ver = CURRENT_MESH_VER;
	header.flags = 0;
	header.length = sizeof(MeshFileHeader) + sizeof(MeshFileMeta);

	MeshFileMeta meta;
	createMeshFileHeader(mesh, header, meta);
	
	const uint vertexBytesLength = mesh.vertexCount * sizeof(vec3);
	
	stream.write(&header, sizeof(header));
	stream.write(&meta, sizeof(meta));

	stream.write(mesh.vertices, vertexBytesLength);
	
	if (mesh.hasNormal) {
		stream.write(mesh.normals, vertexBytesLength);
	}
	
	if (mesh.hasTexcoord) {
		stream.write(mesh.texcoords, mesh.vertexCount * mesh.uvCount * sizeof(vec2));
	}
	
	if (mesh.hasTangentSpaceBasis) {
		stream.write(mesh.tangents, vertexBytesLength);
		stream.write(mesh.bitangents, vertexBytesLength);
	}
	
	if (mesh.hasColor) {
		stream.write(mesh.colors, mesh.vertexCount * sizeof(color3));
	}
	
	if (mesh.indexCount > 0) {
		stream.write(mesh.indexes, mesh.indexCount * sizeof(vertex_index_t));
	}
	
	if (mesh.edgeCount > 0 && mesh.edges != NULL) {
		stream.write(mesh.edges, mesh.edgeCount * sizeof(Edge));
	}
}

void MeshLoader::createMeshFileHeader(const Mesh& mesh, MeshFileHeader& header, MeshFileMeta& meta) {
	
	if (mesh.hasNormal) {
		header.flags |= MeshFileHeaderFlags::MHF_HasNormal;
	}
	
	if (mesh.hasTexcoord) {
		header.flags |= MeshFileHeaderFlags::MHF_HasTexcoord;
	}
	
	if (mesh.hasTangentSpaceBasis) {
		header.flags |= MeshFileHeaderFlags::MHF_HasTangentBasisData;
	}
	
	meta.vertexCount = mesh.vertexCount;
	meta.uvCount = mesh.uvCount;
	meta.indexCount = mesh.indexCount;
	
	if (mesh.hasBoundingBox) {
		header.flags |= MeshFileHeaderFlags::MHF_HasBoundingBox;
		meta.bbox = mesh.bbox;
	}

	if (mesh.hasLightmap) {
		header.flags |= MeshFileHeaderFlags::MHF_HasLightmap;
		meta.lightmapTrunkId = mesh.lightmapTrunkUid;
		meta.lightmapType = MeshLightmapTypes::MLT_JPEG_IMAGE;
	}
	
	if (mesh.hasRefmap) {
		header.flags |= MeshFileHeaderFlags::MHF_HasRefmap;
		meta.refmapTrunkId = mesh.refmapTrunkUid;
	}
	
	if (mesh.hasColor) {
		header.flags |= MeshFileHeaderFlags::MHF_HasColor;
	}
	
	if (mesh.hasGrabBoundary) {
		header.flags |= MeshFileHeaderFlags::MHF_HasGrabBoundary;
		meta.grabBoundary = mesh.grabBoundary;
	}
	
	if (mesh.edgeCount > 0 && mesh.edges != NULL) {
		header.flags |= MeshFileHeaderFlags::MHF_HasWireframe;
		meta.edgeCount = mesh.edgeCount;
	}
}

#undef FORMAT_TAG_MESH
#undef FORMAT_TAG_LMAP
#undef CURRENT_MESH_VER

}