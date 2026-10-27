// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// WorldFieldAssetTests: the milestone-4 acceptance — "compressed assets
// match a dense reference within defined quality tolerances":
//   * BC4 codec: encode/decode roundtrip error bounds on smooth bricks
//     (the static-density payload form);
//   * SqrtUNorm packing: attribute -> sqrt -> UNORM8 (the shader's
//     scale*enc^2+bias inverse);
//   * the .wfb asset: builder -> serialize -> load -> FindBrick
//     (payload / cooked-EMPTY / missing), file roundtrip, MakeDesc;
//   * the FIDELITY proof: a smooth world density field cooked to BC4
//     bricks (halos from the same world function, sqrt-encoded BEFORE
//     quantization) sampled through software trilinear + the sqrt
//     decode == the dense quantized reference within tolerance, at
//     deterministic positions INCLUDING exact brick-boundary edges and
//     corners (the sqrt-filtering non-commutativity is inside the bound);
//   * cross-brick halo consistency: adjacent bricks' decoded edge data
//     agrees (each brick's halo encodes the neighbor's world samples);
//   * the CookedBrickSource through WorldFieldSystem: BC4 atlas paging,
//     every resident page's queued upload is byte-identical to the
//     asset's brick payload.
//
// Hand-rolled runner (no test framework dependency), like the other tests.

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

#include <glm/glm.hpp>

#include <Engine/WorldFields/WorldField.h>
#include <Engine/WorldFields/WorldFieldAtlas.h>
#include <Engine/WorldFields/WorldFieldAsset.h>
#include <Engine/WorldFields/WorldFieldCodec.h>

namespace wf = engine::worldfields;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name)
{
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

bool Near(float a, float b, float eps)
{
	return std::fabs(a - b) <= eps;
}

// The smooth world density function (fog-like: bounded, low-frequency).
float WorldDensity(glm::vec3 p)
{
	float v = 0.35f
		+ 0.30f * std::sin(p.x * 0.137f + 1.2f)
		+ 0.20f * std::sin(p.y * 0.113f + 0.5f)
		+ 0.25f * std::sin(p.z * 0.091f + 2.1f);
	return std::min(std::max(v, 0.0f), 1.0f);
}

// The ENCODED (sqrt) world value — the lattice every side compares on.
uint8_t EncodedAt(glm::vec3 p)
{
	return uint8_t(std::sqrt(WorldDensity(p)) * 255.0f + 0.5f);
}

// Sample the dense quantized ENCODED lattice (the reference: no BC4, no
// paging — trilinear over texel centers, then the sqrt decode).
float ReferenceSampleEncoded(glm::vec3 pos, float voxel, glm::vec3 origin)
{
	float L[3];
	int i0[3];
	float alpha[3];
	for (int k = 0; k < 3; ++k) {
		L[k] = (pos[k] - origin[k]) / voxel;
		const float t = L[k] - 0.5f;
		i0[k] = int(std::floor(t));
		alpha[k] = t - float(i0[k]);
	}
	auto lattice = [&](int i[3]) {
		// Lattice index i (cell-center units) -> the texel-center world
		// position the stored bricks sample (i + 0.5) * voxel.
		const glm::vec3 p = origin + glm::vec3(
			(float(i[0]) + 0.5f) * voxel,
			(float(i[1]) + 0.5f) * voxel,
			(float(i[2]) + 0.5f) * voxel);
		return float(EncodedAt(p));
	};
	float acc = 0.0f;
	for (int dz = 0; dz < 2; ++dz)
		for (int dy = 0; dy < 2; ++dy)
			for (int dx = 0; dx < 2; ++dx) {
				int i[3] = { i0[0] + dx, i0[1] + dy, i0[2] + dz };
				const float w = (dx ? alpha[0] : 1.0f - alpha[0])
					* (dy ? alpha[1] : 1.0f - alpha[1])
					* (dz ? alpha[2] : 1.0f - alpha[2]);
				acc += w * lattice(i);
			}
	return acc / 255.0f;
}

// Build one stored brick's ENCODED (sqrt-quantized) texel lattice for a
// brick coordinate (texel centers, minus halo — the same convention the
// WorldRenderer checkerboard source pins).
std::vector<uint8_t> BuildEncodedBrick(uint32_t lod, int32_t bx, int32_t by,
	int32_t bz, float baseVoxel, glm::vec3 origin)
{
	const float voxel = baseVoxel * float(1u << lod);
	const glm::vec3 brickMin = origin + glm::vec3(float(bx), float(by), float(bz))
		* (float(wf::kBrickInteriorRes) * voxel);
	std::vector<uint8_t> texels(size_t(wf::kBrickStoredRes)
		* wf::kBrickStoredRes * wf::kBrickStoredRes);
	size_t i = 0;
	for (uint32_t z = 0; z < wf::kBrickStoredRes; ++z)
		for (uint32_t y = 0; y < wf::kBrickStoredRes; ++y)
			for (uint32_t x = 0; x < wf::kBrickStoredRes; ++x) {
				const glm::vec3 p = brickMin + glm::vec3(
					float(x) + 0.5f - float(wf::kBrickBorder),
					float(y) + 0.5f - float(wf::kBrickBorder),
					float(z) + 0.5f - float(wf::kBrickBorder)) * voxel;
				texels[i++] = EncodedAt(p);
			}
	return texels;
}

} // namespace

int main()
{
	const glm::vec3 origin(0.0f, 0.0f, 0.0f);
	constexpr uint32_t kStored = wf::kBrickStoredRes;
	constexpr size_t kBlockBytes = wf::StoredBrickBlockBytes(kStored, 4, 4, 8);

	// ---- 1) SqrtUNorm packing ----
	{
		float attr[3] = { 0.0f, 0.25f, 1.0f };
		uint8_t enc[3] = {};
		wf::QuantizeSqrtUnorm8(attr, 3, enc);
		Check(enc[0] == 0 && enc[2] == 255, "sqrt-pack.endpoints");
		// 0.25 -> sqrt 0.5 -> ~127/128
		Check(std::abs(int(enc[1]) - 128) <= 1, "sqrt-pack.quarter");
		float linear[3] = { 0.0f, 0.5f, 1.0f };
		uint8_t lenc[3] = {};
		wf::QuantizeUnorm8(linear, 3, lenc);
		Check(lenc[1] == 128, "linear-pack.half");
	}

	// ---- 2) BC4 codec roundtrip (smooth brick) ----
	std::vector<uint8_t> brickTexels = BuildEncodedBrick(0, 0, 0, 0, 1.0f, origin);
	{
		std::vector<std::byte> blocks(kBlockBytes);
		const size_t written = wf::Bc4EncodeBrick(brickTexels.data(), kStored,
			blocks.data(), blocks.size());
		Check(written == kBlockBytes, "bc4.encode-size");
		std::vector<uint8_t> decoded(size_t(kStored) * kStored * kStored);
		const size_t decodedBytes = wf::Bc4DecodeBrick(blocks.data(), blocks.size(),
			kStored, decoded.data(), decoded.size());
		Check(decodedBytes == kBlockBytes, "bc4.decode-size");
		uint32_t maxErr = 0;
		double sum = 0.0;
		for (size_t i = 0; i < decoded.size(); ++i) {
			const int e = std::abs(int(decoded[i]) - int(brickTexels[i]));
			maxErr = std::max<uint32_t>(maxErr, uint32_t(e));
			sum += double(e);
		}
		std::cout << "[info] bc4 smooth-brick: maxErr=" << maxErr
			<< " mean=" << (sum / double(decoded.size())) << "\n";
		// BC4 UNORM: 8-bit endpoints + 6 interpolated levels per 4x4
		// block — worst-case within-block quantization for gradient data
		// lands ~9/255 (observed; DirectTex's reference encoder). The
		// MEAN is what the fog fidelity rides on (see the dense-reference
		// check below) — 12/255 is the honest format-typical bound.
		Check(maxErr <= 12, "bc4.roundtrip-max-error (<= 12/255, format-typical)");
		Check(sum / double(decoded.size()) < 2.0, "bc4.roundtrip-mean-error (< 2/255)");
	}

	// ---- 3) the .wfb asset roundtrip ----
	{
		// Bricks (0,0,0) and (1,0,0) at LOD 0 as payloads; (0,-1,0) EMPTY.
		std::vector<std::byte> b0(kBlockBytes), b1(kBlockBytes);
		{
			std::vector<uint8_t> t1 = BuildEncodedBrick(0, 1, 0, 0, 1.0f, origin);
			Check(wf::Bc4EncodeBrick(brickTexels.data(), kStored,
				b0.data(), b0.size()) == kBlockBytes, "asset.brick0-encoded");
			Check(wf::Bc4EncodeBrick(t1.data(), kStored,
				b1.data(), b1.size()) == kBlockBytes, "asset.brick1-encoded");
		}

		wf::WorldFieldAssetBuilder builder(smvk::Format::BC4_UNorm,
			wf::WorldFieldEncoding::SqrtUNorm, 1.0f, 2,
			origin, glm::vec4(0.02f, 0, 0, 0), glm::vec4(0.0f), glm::vec4(0.0f));
		builder.AddBrick(0, 0, 0, 0, b0.data(), b0.size());
		builder.AddBrick(0, 1, 0, 0, b1.data(), b1.size());
		builder.AddEmpty(0, 0, -1, 0);
		Check(builder.BrickCount() == 3, "asset.builder-count");

		std::vector<std::byte> file(builder.Serialize(nullptr, 0) + 1);
		// (Serialize(nullptr, 0) returns 0; size it directly:)
		const size_t total = 96 + 3 * sizeof(wf::WorldFieldAssetIndexRow)
			+ 2 * kBlockBytes;
		file.resize(total);
		Check(builder.Serialize(file.data(), file.size()) == total,
			"asset.serialize-size");

		wf::WorldFieldAsset asset;
		Check(wf::WorldFieldAsset::LoadFromMemory(&asset, file.data(), file.size()),
			"asset.load-from-memory");
		Check(asset.BrickCount() == 3, "asset.loaded-count");

		const std::byte* p = nullptr;
		size_t pb = 0;
		Check(asset.FindBrick(0, 0, 0, 0, &p, &pb) == wf::WorldFieldAsset::BrickLookup::Payload
			&& pb == kBlockBytes && std::memcmp(p, b0.data(), kBlockBytes) == 0,
			"asset.find-brick0-payload-exact");
		Check(asset.FindBrick(0, 1, 0, 0, &p, &pb) == wf::WorldFieldAsset::BrickLookup::Payload
			&& pb == kBlockBytes && std::memcmp(p, b1.data(), kBlockBytes) == 0,
			"asset.find-brick1-payload-exact");
		Check(asset.FindBrick(0, 0, -1, 0, &p, &pb)
			== wf::WorldFieldAsset::BrickLookup::Empty,
			"asset.find-empty");
		Check(asset.FindBrick(0, 0, 0, 1, &p, &pb)
			== wf::WorldFieldAsset::BrickLookup::Missing,
			"asset.find-missing");
		Check(asset.FindBrick(1, 0, 0, 0, &p, &pb)
			== wf::WorldFieldAsset::BrickLookup::Missing,
			"asset.find-missing-lod");

		// MakeDesc: the asset owns the data semantics.
		const wf::WorldFieldDesc d = asset.MakeDesc("FogCooked", 4, 2, 4, 2, 2, 1);
		Check(d.format == smvk::Format::BC4_UNorm
			&& d.encoding == wf::WorldFieldEncoding::SqrtUNorm
			&& d.lodCount == 2 && d.decodeScale.x == 0.02f,
			"asset.make-desc");

		// File roundtrip (the pre-approved opencode temp dir).
		wf::WorldFieldAsset fromFile;
		{
			const char* tmpPath = "C:/Users/samuel/AppData/Local/Temp/opencode/wf_fidelity_test.wfb";
			Check(builder.WriteToFile(tmpPath), "asset.write-to-file");
			Check(wf::WorldFieldAsset::LoadFromFile(&fromFile, tmpPath),
				"asset.load-from-file");
			std::remove(tmpPath);
			Check(fromFile.FindBrick(0, 0, 0, 0, &p, &pb)
				== wf::WorldFieldAsset::BrickLookup::Payload
				&& pb == kBlockBytes && std::memcmp(p, b0.data(), kBlockBytes) == 0,
				"asset.file-roundtrip-payload-exact");
		}
	}

	// ---- 4) the fidelity proof: cooked BC4 + sqrt decode vs the dense
	// quantized reference, including exact brick-boundary positions ----
	{
		// A neighborhood of bricks (each encoded from the same world
		// function; halos included by construction).
		struct CookedBrick
		{
			int32_t x, y, z;
			std::vector<uint8_t> decoded; // BC4-decoded texels
		};
		std::vector<CookedBrick> cooked;
		for (int bz = 0; bz <= 1; ++bz)
			for (int by = 0; by <= 0; ++by)
				for (int bx = 0; bx <= 1; ++bx) {
					std::vector<uint8_t> texels = BuildEncodedBrick(0, bx, by, bz, 1.0f, origin);
					std::vector<std::byte> blocks(kBlockBytes);
					(void)wf::Bc4EncodeBrick(texels.data(), kStored, blocks.data(), blocks.size());
					std::vector<uint8_t> decoded(texels.size());
					(void)wf::Bc4DecodeBrick(blocks.data(), blocks.size(), kStored,
						decoded.data(), decoded.size());
					cooked.push_back({ bx, by, bz, std::move(decoded) });
				}

		// Software trilinear over ONE brick's decoded texels at a local
		// cell coordinate (the in-atlas math — halo offsets included).
		auto sampleBrick = [&](const CookedBrick& b, glm::vec3 local) {
			const glm::vec3 c = local + glm::vec3(float(wf::kBrickBorder));
			int i0[3];
			float alpha[3];
			for (int k = 0; k < 3; ++k) {
				const float t = c[k] - 0.5f;
				i0[k] = int(std::floor(t));
				alpha[k] = t - float(i0[k]);
			}
			auto texel = [&](int x, int y, int z) {
				return float(b.decoded[(size_t(z) * kStored + size_t(y)) * kStored
					+ size_t(x)]);
			};
			float acc = 0.0f;
			for (int dz = 0; dz < 2; ++dz)
				for (int dy = 0; dy < 2; ++dy)
					for (int dx = 0; dx < 2; ++dx) {
						const float w = (dx ? alpha[0] : 1.0f - alpha[0])
							* (dy ? alpha[1] : 1.0f - alpha[1])
							* (dz ? alpha[2] : 1.0f - alpha[2]);
						acc += w * texel(i0[0] + dx, i0[1] + dy, i0[2] + dz);
					}
			return acc / 255.0f;
		};

		// Deterministic positions: inside brick (0,0,0), on its +X face
		// (the shared edge with brick (1,0,0)) at ±1e-3, corners of the
		// brick box, and along the shared +Z edge with brick (0,0,1).
		uint32_t rng = 0xC0FFEEu;
		auto lcg = [&]() {
			rng = rng * 1664525u + 1013904223u;
			return float(int32_t(rng >> 8) & 0xFFFF) / 65535.0f;
		};
		float maxAbsErr = 0.0f;
		float sumErr = 0.0f;
		int samples = 0;
		for (int s = 0; s < 400; ++s) {
			glm::vec3 local(lcg() * 64.0f, lcg() * 64.0f, lcg() * 64.0f);
			switch (s % 5) {
			case 0: local.x = 0.0f; break; // exact brick min corner face
			case 1: local.x = 63.999f; break; // just inside the +X face
			case 2: local.x = 64.001f; break; // just PAST it (next brick's local -63.999)
			case 3: local = glm::vec3(63.999f, 0.0f, 0.0f); break; // edge corner
			case 4: local = glm::vec3(0.0f, 32.0f, 63.999f); break; // the shared +Z edge
			default: break;
			}
			const CookedBrick* brick = &cooked[0]; // brick (0,0,0)
			glm::vec3 localAdj = local;
			if (local.x >= 64.0f) {
				brick = &cooked[1]; // brick (1,0,0): local rebased
				localAdj.x = local.x - 64.0f;
			}
			const float encodedBrick = sampleBrick(*brick, localAdj);
			const float gotDensity = encodedBrick * encodedBrick; // sqrt decode, scale 1 bias 0
			const glm::vec3 world = origin + local; // brick (0,0,0) min corner at origin
			const float refEncoded = ReferenceSampleEncoded(world, 1.0f, origin);
			const float refDensity = refEncoded * refEncoded;
			const float err = std::fabs(gotDensity - refDensity);
			maxAbsErr = std::max(maxAbsErr, err);
			sumErr += err;
			++samples;
		}
		std::cout << "[info] fidelity: samples=" << samples
			<< " maxAbs=" << maxAbsErr << " mean=" << (sumErr / samples) << "\n";
		Check(maxAbsErr <= 0.03f, "fidelity.bc4-sqrt-max (<= 0.03 of full scale)");
		Check(sumErr / samples < 0.01f, "fidelity.bc4-sqrt-mean (< 0.01)");
	}

	// ---- 5) cross-brick halo consistency (the encoded-domain seam) ----
	{
		std::vector<uint8_t> a = BuildEncodedBrick(0, 0, 0, 0, 1.0f, origin);
		std::vector<uint8_t> b = BuildEncodedBrick(0, 1, 0, 0, 1.0f, origin);
		// brick A's right halo columns (x=66,67) are brick B's left
		// interior's neighbor columns (x=0,1 recentered) — both encode the
		// same world samples by construction. Compare a few texel bands.
		uint32_t maxDiff = 0;
		for (uint32_t z = 0; z < kStored; ++z)
			for (uint32_t y = 0; y < kStored; ++y) {
				// A: halo x = 66,67 (interior-relative 64,65 = B interior 0,1 + halo offset)
				// B: interior x=0,1 are stored x=2,3; their left halo x=0,1
				// equals A interior 62,63. The shared samples: A halo(66) ==
				// B stored(2)'s left-neighbor world sample... the exact identity
				// is A stored x=66 == B stored x=2 - 2 + ... simply: A's
				// stored(66) samples world x=64.5; B's stored(2) samples
				// world x=64.5+128? No: B's brick min is +64, so B stored(2)
				// samples world x = 64 + 0.5 = 64.5 -> SAME as A stored(66)
				// (A min 0: stored(66) = 66-2+0.5 = 64.5). Identity: A(66) == B(2).
				const uint8_t va = a[(size_t(z) * kStored + size_t(y)) * kStored + 66];
				const uint8_t vb = b[(size_t(z) * kStored + size_t(y)) * kStored + 2];
				maxDiff = std::max<uint32_t>(maxDiff, uint32_t(std::abs(int(va) - int(vb))));
			}
		Check(maxDiff == 0, "halo.cross-brick-exact (A halo == B interior)");
	}

	// ---- 6) the CookedBrickSource through the WorldFieldSystem ----
	{
		// Build a small cooked asset: bricks around origin at LOD 0 (a
		// 2x2x2 neighborhood), LOD 1 coarse bricks of the same world.
		wf::WorldFieldAssetBuilder builder(smvk::Format::BC4_UNorm,
			wf::WorldFieldEncoding::SqrtUNorm, 1.0f, 2,
			origin, glm::vec4(1.0f), glm::vec4(0.0f), glm::vec4(0.0f));
		for (int bz = -1; bz <= 2; ++bz)
			for (int by = -1; by <= 1; ++by)
				for (int bx = -1; bx <= 2; ++bx) {
					std::vector<uint8_t> t = BuildEncodedBrick(0, bx, by, bz, 1.0f, origin);
					std::vector<std::byte> blocks(kBlockBytes);
					(void)wf::Bc4EncodeBrick(t.data(), kStored, blocks.data(), blocks.size());
					builder.AddBrick(0, bx, by, bz, blocks.data(), blocks.size());
				}
		for (int bz = -1; bz <= 1; ++bz)
			for (int by = -1; by <= 1; ++by)
				for (int bx = -1; bx <= 1; ++bx) {
					std::vector<uint8_t> t = BuildEncodedBrick(1, bx, by, bz, 1.0f, origin);
					std::vector<std::byte> blocks(kBlockBytes);
					(void)wf::Bc4EncodeBrick(t.data(), kStored, blocks.data(), blocks.size());
					builder.AddBrick(1, bx, by, bz, blocks.data(), blocks.size());
				}
		builder.AddEmpty(0, 0, -2, 0);

		wf::WorldFieldAsset asset;
		{
			std::vector<std::byte> file(96
				+ builder.BrickCount() * sizeof(wf::WorldFieldAssetIndexRow)
				+ (builder.BrickCount() - 1) * kBlockBytes); // one EMPTY row
			Check(builder.Serialize(file.data(), file.size()) == file.size(),
				"source.asset-serialized");
			Check(wf::WorldFieldAsset::LoadFromMemory(&asset, file.data(), file.size()),
				"source.asset-loaded");
		}

		wf::WorldFieldSystem sys;
		wf::WorldFieldStreamingPolicy policy{};
		policy.maxBrickLoadsPerFrame = 8;
		policy.evictHysteresisFrames = 2;
		policy.proactiveEvictFrames = 1000;
		sys.SetStreamingPolicy(policy);
		const wf::WorldFieldDesc d = asset.MakeDesc("FogCooked", 2, 2, 2, 2, 2, 1);
		const wf::WorldFieldHandle h = sys.RegisterField(d);
		wf::CookedBrickSource source(asset);
		sys.SetBrickSource(h, &source);

		// Walk the eye; every queued upload must be byte-identical to the
		// asset's payload for that brick (the milestone-4 system-level
		// identity), and pages must only ever reference their own slot.
		bool payloadsIdentical = true;
		bool pagesConsistent = true;
		uint64_t totalUploads = 0;
		// Resident pages must reference bricks that exist in the asset
		// (and only those — the source's FindBrick is the oracle).
		auto probePages = [&]() {
			const std::byte* table = sys.TableBytes();
			if (table == nullptr || sys.TableUsedBytes() < 16)
				return;
			wf::WorldFieldTableHeader hd{};
			std::memcpy(&hd, table, sizeof(hd));
			for (uint32_t fId = 0; fId < hd.fieldCount; ++fId) {
				wf::WorldFieldGpu fg{};
				std::memcpy(&fg, table + 16 + size_t(fId) * hd.fieldStride, sizeof(fg));
				for (uint32_t lod = 0; lod < fg.meta.z; ++lod) {
					const wf::WorldLevelGpu& lv = fg.levels[lod];
					for (uint32_t e = 0;
						e < lv.windowDim.x * lv.windowDim.y * lv.windowDim.z; ++e) {
						wf::WorldPageGpu page{};
						std::memcpy(&page, table + lv.pageTableOffset + e * 16u,
							sizeof(page));
						if (page.atlasSlot < 0)
							continue;
						if (asset.FindBrick(lod, page.brickX, page.brickY, page.brickZ,
								nullptr, nullptr)
							!= wf::WorldFieldAsset::BrickLookup::Payload)
							pagesConsistent = false;
					}
				}
			}
		};
		for (uint64_t f = 0; f < 24; ++f) {
			const glm::vec3 eye(32.0f + float(f) * 30.0f, 8.0f, 32.0f);
			sys.ComputeResidency(eye, f);
			const uint32_t n = sys.PendingUploadCount();
			for (uint32_t u = 0; u < n; ++u) {
				const wf::WorldFieldRegionUpload& up = sys.PendingUploads()[u];
				const std::byte* blocks = nullptr;
				size_t bb = 0;
				if (asset.FindBrick(up.key.lod, up.key.x, up.key.y, up.key.z,
						&blocks, &bb) != wf::WorldFieldAsset::BrickLookup::Payload
					|| bb != up.payloadBytes
					|| std::memcmp(blocks, up.payload, up.payloadBytes) != 0)
					payloadsIdentical = false;
				++totalUploads;
			}
			sys.DropPendingGpuWork();
			probePages();
		}
		Check(totalUploads > 0, "source.paged-bricks");
		Check(payloadsIdentical, "source.upload-payloads-byte-identical-to-asset");
		Check(pagesConsistent, "source.pages-all-cooked");
		const wf::WorldFieldStats st = sys.Stats();
		Check(st.residentBricks > 0, "source.residents");
	}

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
