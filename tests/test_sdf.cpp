// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/sdf.h>
#include <Engine/Asset/pak.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

static int failures = 0;
static void Check(bool ok, const char* name) {
	if (!ok) { std::cerr << "FAIL: " << name << "\n"; ++failures; }
}

int main() {
	using namespace tour::asset;
	// Outward-facing closed cube, with two triangles per side.
	const std::vector<glm::vec3> p = {
		{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
		{-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1}};
	const std::vector<uint32_t> tris = {
		0,2,1, 0,3,2, 4,5,6, 4,6,7,
		0,1,5, 0,5,4, 1,2,6, 1,6,5,
		2,3,7, 2,7,6, 3,0,4, 3,4,7};
	SdfBakeOptions opt;
	opt.resolution = 16;
	opt.bandVoxels = 6;
	std::string err;
	SdfVolume v;
	Check(BakeMeshSdf(p.data(),p.size(),tris.data(),tris.size(),opt,v,err),
		"closed cube bake");
	Check(v.signedDistance, "cube has watertight sign");
	Check(v.samples.size()==16*16*16, "voxel count");
	auto index=[](int x,int y,int z) {return size_t(x+16*(y+16*z));};
	Check(v.samples[index(8,8,8)]<0, "cube center is inside");
	Check(v.samples[index(0,0,0)]>0, "outside corner positive");
	Check(v.voxelSize>0 && v.maxDistance>0, "world-space metrics");
	SdfAsset a;
	a.meshes.push_back({"mesh.0/prim.0",v});
	std::vector<uint8_t> bytes = WriteSdfAsset(a);
	SdfAsset decoded;
	Check(ParseSdfAsset(bytes.data(),bytes.size(),decoded,err), "SDF roundtrip");
	Check(decoded.meshes.size()==1 && decoded.meshes[0].volume.samples==v.samples
		&& decoded.meshes[0].fragment=="mesh.0/prim.0", "SDF contents");
	SdfAsset unchanged = decoded;
	bytes[0]='X';
	Check(!ParseSdfAsset(bytes.data(),bytes.size(),decoded,err), "reject corrupted magic");
	Check(decoded.meshes.size()==unchanged.meshes.size(), "parse failure transactional");
	bytes=WriteSdfAsset(a);
	bytes.pop_back();
	Check(!ParseSdfAsset(bytes.data(),bytes.size(),decoded,err), "reject truncated samples");
	std::vector<uint32_t> open(tris.begin(),tris.begin()+3);
	Check(BakeMeshSdf(p.data(),p.size(),open.data(),open.size(),opt,v,err)
		&& !v.signedDistance, "open triangle unsigned fallback");
	for(auto sample : v.samples) Check(sample>=0,"unsigned samples nonnegative");
	SdfBakeOptions invalid=opt;
	invalid.resolution=4;
	Check(!BakeMeshSdf(p.data(),p.size(),tris.data(),tris.size(),invalid,v,err),
		"reject invalid resolution");
	std::vector<uint32_t> broken={0,1,900};
	Check(!BakeMeshSdf(p.data(),p.size(),broken.data(),broken.size(),opt,v,err),
		"reject invalid indices");
	// Pak kind and serving contract: no texture envelope is applied.
	PakWriter writer;
	bytes=WriteSdfAsset(a);
	Check(writer.AddBlob(PakAssetKind::Sdf,"content/models/cube.geometry.sdf",bytes),
		"pak embeds sdf kind");
	std::vector<uint8_t> pak=writer.Finalize();
	auto reader=PakReader::Open(pak,err);
	Check(bool(reader),"pak opens");
	if(reader) {
		auto entry=reader->Find(PakAssetKind::Sdf,
			"content/models/cube.geometry.sdf");
		Check(entry!=nullptr,"pak sdf lookup");
		if(entry) {
			auto raw=PakServeVfsBytes(PakAssetKind::Sdf,
				reader->Payload(*entry),"content/models/cube.geometry.sdf");
			Check(raw.has_value() && *raw==bytes,"pak serves verbatim sdf bytes");
		}
	}
	return failures ? 1:0;
}
