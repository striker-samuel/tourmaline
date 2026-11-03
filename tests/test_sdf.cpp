// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#include <Engine/Asset/sdf.h>
#include <Engine/Asset/sdf_reflections.h>
#include <Engine/Renderer/static_sdf_gpu.h>
#include <Engine/Asset/pak.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <glm/vec4.hpp>

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
	const auto packedSamples = smvk::PackStaticSdfWords(v.samples);
	Check(packedSamples.size() == (v.samples.size() + 1) / 2,
		"static SDF GPU word count");
	for (size_t i = 0; i < v.samples.size(); ++i) {
		const uint32_t shift = uint32_t(i & 1u) * 16u;
		const uint16_t bits = uint16_t(packedSamples[i / 2] >> shift);
		Check(std::bit_cast<int16_t>(bits) == v.samples[i],
			"static SDF GPU sample bit-for-bit parity");
	}
	const std::array<int16_t, 3> oddSamples{-32767, 32767, -1};
	const auto oddPacked = smvk::PackStaticSdfWords(oddSamples);
	Check(oddPacked.size() == 2 && oddPacked[0] == 0x7fff8001u &&
		oddPacked[1] == 0x0000ffffu,
		"static SDF odd word padding and signed-endian packing");
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

	// Static-only geometric reflection queries against baked TSDF data.
	// No renderer/GPU state is involved. The nearest instance wins and
	// all transforms remain mesh-local -> world (including nonuniform scale).
	StaticSdfTraceOptions trace{};
	trace.maxDistance = 20.0f;
	trace.hitEpsilon = 0.01f;
	StaticSdfHit hit{};
	StaticSdfInstance cube{&a.meshes[0].volume, glm::mat4(1.0f)};
	Check(TraceStaticSdfRay(std::span(&cube, 1),
		glm::vec3(-4, 0, 0), glm::vec3(1, 0, 0), trace, hit) &&
		std::abs(hit.distance - 3.0f) < 0.2f &&
		hit.normal.x < -0.9f, "static SDF ray hits cube front face");

	StaticSdfInstance scaled = cube;
	scaled.meshToWorld[0][0] = 2.0f;
	scaled.meshToWorld[1][1] = 0.5f;
	scaled.meshToWorld[2][2] = 1.5f;
	scaled.meshToWorld[3] = glm::vec4(5.0f, 0.0f, 0.0f, 1.0f);
	Check(TraceStaticSdfRay(std::span(&scaled, 1),
		glm::vec3(0, 0, 0), glm::vec3(2, 0, 0), trace, hit) &&
		std::abs(hit.distance - 3.0f) < 0.3f &&
		hit.normal.x < -0.9f, "nonuniform scale keeps world distance and normal");
	const std::array<StaticSdfInstance, 2> instances{ scaled, cube };
	Check(TraceStaticSdfRay(instances,
		glm::vec3(-6, 0, 0), glm::vec3(1, 0, 0), trace, hit) &&
		hit.instanceIndex == 1 && std::abs(hit.distance - 5.0f) < 0.2f,
		"nearest static SDF instance wins");

	StaticSdfInstance reflected = cube;
	reflected.meshToWorld[3] = glm::vec4(-5.0f, 0.0f, 0.0f, 1.0f);
	Check(TraceStaticSdfReflection(std::span(&reflected, 1),
		glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0),
		trace, hit) && std::abs(hit.distance - 4.0f) < 0.2f &&
		std::abs(hit.position.x + 4.0f) < 0.2f,
		"static reflection ray bounces toward off-screen SDF object");

	Check(TraceStaticSdfRay(std::span(&cube, 1),
		glm::vec3(0), glm::vec3(1, 0, 0), trace, hit) &&
		std::abs(hit.distance - 1.0f) < 0.2f,
		"signed SDF rays starting inside find the exit surface");
	const StaticSdfHit previous = hit;
	Check(!TraceStaticSdfRay(std::span(&cube, 1),
		glm::vec3(0, 3, 0), glm::vec3(1, 0, 0), trace, hit) &&
		hit.distance == previous.distance, "miss does not alter previous hit");
	trace.maxDistance = 1.0f;
	Check(!TraceStaticSdfRay(std::span(&scaled, 1),
		glm::vec3(0), glm::vec3(1, 0, 0), trace, hit),
		"maximum reflection distance bounds the ray");
	trace.maxDistance = 20.0f;
	StaticSdfInstance singular = cube;
	singular.meshToWorld[1][1] = 0.0f;
	Check(!TraceStaticSdfRay(std::span(&singular, 1),
		glm::vec3(-4, 0, 0), glm::vec3(1, 0, 0), trace, hit),
		"singular transform is ignored");
	Check(!TraceStaticSdfRay(std::span(&cube, 1),
		glm::vec3(-4, 0, 0), glm::vec3(0), trace, hit),
		"zero direction is rejected");
	std::vector<uint32_t> open(tris.begin(),tris.begin()+3);
	Check(BakeMeshSdf(p.data(),p.size(),open.data(),open.size(),opt,v,err)
		&& !v.signedDistance, "open triangle unsigned fallback");
	for(auto sample : v.samples) Check(sample>=0,"unsigned samples nonnegative");

	StaticSdfInstance openInstance{&v, glm::mat4(1.0f)};
	Check(TraceStaticSdfRay(std::span(&openInstance, 1),
		glm::vec3(0, 0, -4), glm::vec3(0, 0, 1), trace, hit) &&
		std::abs(hit.distance - 3.0f) < 0.25f,
		"unsigned static SDF traces an open surface without interior sign");

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
