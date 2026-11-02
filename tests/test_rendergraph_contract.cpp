// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// RendergraphContractTests: CPU-testable frame-renderer contracts.
// The 14k-line WorldRenderer + 11k-line Vulkan impl have no GPU unit tests;
// these pin the parts that are pure logic so refactors fail fast:
//
//   1. Overlay-scratch discipline: every VS tail push lives in bytes
//      192..256 of IRenderer::PushConstants and fits (filtered 48B at 192
//      — 32B base + the 16B VS-skin tail at 224, whose bytes the mesh tail
//      re-pushes: mutually exclusive legs — mesh tail 16B at 224, uvflat
//      16B at 192, placement 32B at 192).
//   2. Mesh eligibility: MeshDrawEligible truth table (opaque-only) —
//      the CPU partition feeds DrawMeshTasks exactly the
//      ranges the indexed loop skips (exactly-once).
//   3. GPU-driven culling is the only path (no kill switch, no legacy
//      CPU regime); RunConfig mesh default ON.
//   4. Filtered/motion tail prefix identity: FilteredVsPush's first 24B
//      (draws/renderViews/renderViewIndex at 192..216) match the motion
//      VS tail's reads (documented in gfx_filter.h:348-359).
//
// Heavier coverage (barrier emission, cull-builder differentials) follows
// the test_gfxfilter.cpp pattern; headless goldens via
// tools/golden_capture.py (TOURMALINE_CAPTURE + --bench + PNG hash).

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>

#include <Engine/App/RunConfig.h>
#include <Engine/Placement/placement_draw.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/Renderer.h>
#include <Engine/Renderer/WorldRenderer.h> // FrameUniforms + the §1.1 layer-table pack contract
#include <Engine/Renderer/gfx_filter.h>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	if (++g_checks, cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

} // namespace

// 5. TopologicalSortUsages self-edge contracts (the review's finding):
// pass-dependency edges connect DISTINCT passes only. A single pass can
// declare the same PHYSICAL resource several times (two logical handles
// over one resolved buffer, aliased attachments) - PassUsage is a
// vector, nothing deduplicates it - and the old per-physical edge
// generation inserted src == i self-edges on every branch except image
// WAW, tripping the cycle fallback (the debug assert + a silent
// insertion-order sort). The core now combines each pass's declarations
// by physical resource (write wins) and guards every insert.
void TestTopoSortSelfEdges() {
	using smvk::PassUsage;
	// Fabricated PHYSICAL handles (Handle::Mint - the cook-only
	// fabricator; the sorter only reads index/gen, never a pool).
	const auto physBuf = smvk::BufferHandle::Mint(7, 1);
	const auto physImg = smvk::ImageHandle::Mint(9, 1);
	smvk::BufferSyncRequest bufRead{}, bufWrite{};
	bufRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
	bufWrite.barrierMasks.accessMask = smvk::AccessFlags::ShaderWrite;
	smvk::ImageSyncRequest imgRead{}, imgWrite{};
	imgRead.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead;
	imgWrite.barrierMasks.accessMask = smvk::AccessFlags::ColorAttachmentWrite;
	auto orderIsIdentity = [](const std::vector<size_t>& o) {
		for (size_t i = 0; i < o.size(); ++i)
			if (o[i] != i) return false;
		return true;
	};
	// (a) the reviewer's exact scenario: buffer read + write of ONE
	// physical through two logical handles in a single pass, plus a
	// later reader (the pass-0 write must order before the pass-1
	// read - identity order, no cycle).
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(2);
		u[0].buffers.emplace_back(physBuf, bufRead);  // logical A
		u[0].buffers.emplace_back(physBuf, bufWrite); // logical B, same physical
		u[1].buffers.emplace_back(physBuf, bufRead);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.alias-buf-read-write-no-cycle");
		Check(order.size() == 2 && orderIsIdentity(order),
			"topo.alias-buf-order");
	}
	// (b) buffer write + write of one physical in one pass (the WAW
	// self-edge class).
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(2);
		u[0].buffers.emplace_back(physBuf, bufWrite);
		u[0].buffers.emplace_back(physBuf, bufWrite);
		u[1].buffers.emplace_back(physBuf, bufRead);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.alias-buf-write-write-no-cycle");
		Check(order.size() == 2 && orderIsIdentity(order),
			"topo.alias-buf-ww-order");
	}
	// (c) image read + write of one physical in one pass (the WAR
	// readers-loop class), plus a later writer + reader chain.
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(3);
		u[0].images.emplace_back(physImg, imgRead);
		u[0].images.emplace_back(physImg, imgWrite);
		u[1].images.emplace_back(physImg, imgRead);
		u[2].images.emplace_back(physImg, imgWrite);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.alias-img-read-write-no-cycle");
		Check(order.size() == 3 && orderIsIdentity(order),
			"topo.alias-img-order");
	}
	// (d) image write then read of one physical in one pass (the RAW
	// class).
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(2);
		u[0].images.emplace_back(physImg, imgWrite);
		u[0].images.emplace_back(physImg, imgRead);
		u[1].images.emplace_back(physImg, imgRead);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.alias-img-write-read-no-cycle");
		Check(order.size() == 2 && orderIsIdentity(order),
			"topo.alias-img-wr-order");
	}
	// (e) sanity: an ordinary write -> read chain across passes sorts
	// every pass, no cycle, writer first.
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(3);
		u[1].buffers.emplace_back(physBuf, bufWrite);
		u[2].buffers.emplace_back(physBuf, bufRead);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.chain-no-cycle");
		Check(order.size() == 3, "topo.chain-covers-all");
		// pass 1 (writer) before pass 2 (reader).
		size_t pos1 = SIZE_MAX, pos2 = SIZE_MAX;
		for (size_t i = 0; i < order.size(); ++i) {
			if (order[i] == 1) pos1 = i;
			if (order[i] == 2) pos2 = i;
		}
		Check(pos1 < pos2, "topo.chain-writer-first");
	}
	// (f) distinct physical resources in one pass: no false edges, the
	// insertion order stands.
	{
		smvk::TopoSortScratch s;
		std::vector<PassUsage> u(2);
		u[0].buffers.emplace_back(smvk::BufferHandle::Mint(1, 1), bufWrite);
		u[0].buffers.emplace_back(smvk::BufferHandle::Mint(2, 1), bufRead);
		u[1].buffers.emplace_back(smvk::BufferHandle::Mint(3, 1), bufRead);
		const auto& order = smvk::TopologicalSortUsages(u, s);
		Check(!s.hadCycle, "topo.distinct-no-cycle");
		Check(order.size() == 2 && orderIsIdentity(order),
			"topo.distinct-order");
	}
}

// 6. The (stage x access) pairing contract (the stranded-upload
// investigation's second defect): every access bit a barrier declares
// must be supported by at least one stage in the same barrier's stage
// mask. The engine-specific sharp edge: AllGraphics maps to the
// SHADER-stage union ONLY (never DRAW_INDIRECT), so IndirectCommandRead
// @ AllGraphics leaves the fixed-function indirect-command fetch
// unsynchronized - the renderer's rule is IndirectCommandRead @
// PipelineStageBit_DrawIndirect (the ingestion choke points assert it
// via AssertStageAccessSupported; these checks pin the DECISION TABLE
// the assert consults, CPU-testable in every build config).
void TestStageAccessContract() {
	const auto drawIndirect = smvk::SupportingStagesFor(
		smvk::AccessFlags::IndirectCommandRead);
	Check(drawIndirect == smvk::PipelineStageBit_DrawIndirect,
		"pair.indirect-requires-drawindirect");
	// THE hole itself: the shader-stage union supports nothing about the
	// indirect fetch (an AllGraphics pairing asserts in Debug builds).
	Check((smvk::PipelineStageBit_AllGraphics & drawIndirect) == 0,
		"pair.allgraphics-excludes-indirect");
	// The legal indirect pairings: DrawIndirect alone, or unioned with a
	// shader stage for the mesh-shader storage read of the same buffer
	// (GfxRenderer's Main-pass hint shape).
	Check((smvk::PipelineStageBit_DrawIndirect & drawIndirect) != 0,
		"pair.drawindirect-supports-indirect");
	Check(((smvk::PipelineStageBit_Mesh | smvk::PipelineStageBit_DrawIndirect)
		& drawIndirect) != 0,
		"pair.mesh-plus-drawindirect-supports-indirect");
	// IndexRead/VertexAttributeRead: the Vertex bit expands to
	// VERTEX_INPUT in the translators, so AllGraphics covers them (the
	// terrain/probeball hints are the shipped shape).
	const auto vtx = smvk::SupportingStagesFor(smvk::AccessFlags::IndexRead);
	Check((smvk::PipelineStageBit_AllGraphics & vtx) != 0,
		"pair.allgraphics-supports-indexread");
	Check((smvk::PipelineStageBit_Vertex
		& smvk::SupportingStagesFor(smvk::AccessFlags::VertexAttributeRead)) != 0,
		"pair.vertex-supports-vbattrib");
	// ShaderRead/Write: any shader/compute/RT stage (the broad union).
	const auto shader = smvk::SupportingStagesFor(smvk::AccessFlags::ShaderRead);
	Check((smvk::PipelineStageBit_All & shader) == shader,
		"pair.all-supports-shaderread");
	// Transfer + attachment classes.
	Check(smvk::SupportingStagesFor(smvk::AccessFlags::TransferWrite)
		== smvk::PipelineStageBit_Transfer,
		"pair.transfer-requires-transfer-stage");
	Check((smvk::PipelineStageBit_Fragment
		& smvk::SupportingStagesFor(smvk::AccessFlags::ColorAttachmentWrite)) != 0,
		"pair.fragment-supports-colorattach");
	// Stage-agnostic / inexpressible bits skip validation (0 = no
	// engine stage set to check against).
	Check(smvk::SupportingStagesFor(smvk::AccessFlags::MemoryRead) == 0,
		"pair.memoryread-skips-validation");
	Check(smvk::SupportingStagesFor(smvk::AccessFlags::HostRead) == 0,
		"pair.hostread-not-expressible");
	// The assert's no-fire path (live in Debug test builds): a valid
	// hand-written union must pass silently.
	smvk::AssertStageAccessSupported(
		smvk::PipelineStageBit_Mesh | smvk::PipelineStageBit_DrawIndirect,
		smvk::AccessFlags::ShaderRead | smvk::AccessFlags::IndirectCommandRead,
		"RendergraphContractTests");
}

// 5. GI volume layer table (§1.1, docs/gi_volume_perf_investigation.md):
//    the per-volume layer pack is 10 slots (9 BC6H / 7 legacy + invalid
//    tail) over up to 8 volumes — 80 slots per set. The legacy [56]
//    FrameUniforms fields could not hold that (the sixth volume's tail
//    wrote past them); this pins the replacement capacity/stride contract
//    for BOTH sets, including the last legal index (79) the old arrays
//    could never reach.
void TestGiVolumeLayerTableContract() {
	using engine::FrameUniforms;
	using engine::PackGiVolumeLayerSlots;
	using engine::GiVolumeLayerSlotIndex;
	using engine::kGiVolumeLayerStride;
	using engine::kGiVolumeLayerSlots;
	using engine::kGiVolumeMaxVolumes;
	using engine::kGiVolumeLayerInvalid;

	// The capacity contract itself: 8 volumes x the 10-slot stride.
	Check(kGiVolumeLayerStride == 10, "givoltbl.stride-10");
	Check(kGiVolumeMaxVolumes == 8, "givoltbl.volumes-8");
	Check(kGiVolumeLayerSlots == 80, "givoltbl.slots-80");
	// The last legal slot the shader can compute (volume 7, layer 9) —
	// the exact index the legacy [56] arrays truncated.
	Check(GiVolumeLayerSlotIndex(7, 9) == 79, "givoltbl.last-legal-index-79");
	Check(GiVolumeLayerSlotIndex(0, 0) == 0, "givoltbl.first-index-0");
	// The tables actually carry the capacity (the reserved [56] fields
	// stay declared for ABI stability; only these are indexed).
	Check(sizeof(FrameUniforms::giVolumeLayerTable) == 80 * sizeof(uint32_t),
		"givoltbl.table-bytes-320");
	Check(sizeof(FrameUniforms::giVolumeLayerTableB) == 80 * sizeof(uint32_t),
		"givoltbl.b-table-bytes-320");

	// Deterministic pack over every volume count that matters: below the
	// old 56-slot horizon, exactly at the overflow start (volume 5's tail
	// reaches slot 56), and the full 8-volume pack. Both formats.
	for (const uint32_t count : {0u, 1u, 5u, 6u, 8u}) {
		uint32_t tableA[kGiVolumeLayerSlots];
		uint32_t tableB[kGiVolumeLayerSlots];
		for (uint32_t& slot : tableA) slot = 0xAAAAAAAAu;
		for (uint32_t& slot : tableB) slot = 0xAAAAAAAAu;
		for (const uint32_t format : {0u, 1u}) {
			const uint32_t layerCount = format ? 9u : 7u;
			for (uint32_t v = 0; v < count; ++v) {
				uint32_t slots[10];
				for (uint32_t t = 0; t < layerCount; ++t)
					slots[t] = 0x1000u + v * 16u + t; // distinct per volume+layer
				PackGiVolumeLayerSlots(tableA, v, slots, layerCount);
				PackGiVolumeLayerSlots(tableB, v, slots, layerCount);
			}
			char name[96];
			// Valid layers land verbatim; the tail invalidates; volumes
			// beyond the count keep whatever invalidation the caller
			// seeded (the population clears whole tables first).
			bool ok = true;
			for (uint32_t v = 0; v < count && ok; ++v) {
				for (uint32_t t = 0; t < layerCount; ++t)
					ok = ok && tableA[GiVolumeLayerSlotIndex(v, t)]
						== 0x1000u + v * 16u + t
						&& tableB[GiVolumeLayerSlotIndex(v, t)]
						== 0x1000u + v * 16u + t;
				for (uint32_t t = layerCount; t < kGiVolumeLayerStride; ++t)
					ok = ok && tableA[GiVolumeLayerSlotIndex(v, t)]
						== kGiVolumeLayerInvalid
						&& tableB[GiVolumeLayerSlotIndex(v, t)]
						== kGiVolumeLayerInvalid;
			}
			std::snprintf(name, sizeof(name),
				"givoltbl.pack-%u-vols-fmt%u", count, format);
			Check(ok, name);
			if (count > 0) {
				// The last volume's last LEGAL layer index is in-bounds
				// and was written (the §1.1 regression: index 79 at 8).
				const uint32_t last =
					GiVolumeLayerSlotIndex(count - 1, layerCount - 1);
				std::snprintf(name, sizeof(name),
					"givoltbl.last-written-%u-fmt%u", count, format);
				Check(last < kGiVolumeLayerSlots
					&& tableA[last] != kGiVolumeLayerInvalid
					&& tableB[last] != kGiVolumeLayerInvalid, name);
			}
		}
		// The volume that previously overflowed: volume 5's block (50..59)
		// lives INSIDE the new table — slot 56 is a table member now, not
		// a write past the array into whatever field followed it.
		Check(tableA[56] == (count > 5 ? 0x1000u + 5 * 16u + 6u : 0xAAAAAAAAu),
			"givoltbl.overrun-slot-56-contained");
	}
}

int main() {
	using PC = engine::IRenderer::PushConstants;
	constexpr size_t kScratchStart = 192;
	constexpr size_t kScratchEnd = 256;

	// 1. Overlay-scratch discipline.
	Check(smvk::gfx::kFilteredVsPushOffset == kScratchStart, "contract.filtered-offset-192");
	Check(sizeof(smvk::gfx::FilteredVsPush) == 48, "contract.filtered-size-48");
	Check(smvk::gfx::kFilteredVsPushOffset + sizeof(smvk::gfx::FilteredVsPush) <= kScratchEnd,
		"contract.filtered-fits-scratch");
	Check(smvk::gfx::kMeshTailPushOffset == 224, "contract.meshtail-offset-224");
	Check(sizeof(smvk::gfx::MeshTailPush) == 16, "contract.meshtail-size-16");
	Check(smvk::gfx::kMeshTailPushOffset + sizeof(smvk::gfx::MeshTailPush) <= kScratchEnd,
		"contract.meshtail-fits-scratch");
	Check(smvk::gfx::kUvFlatVsPushOffset == kScratchStart, "contract.uvflat-offset-192");
	Check(sizeof(smvk::gfx::UvFlatVsPushTail) == 16, "contract.uvflat-size-16");
	Check(smvk::placement::kPlacementVsPushOffset == kScratchStart,
		"contract.placement-offset-192");
	Check(sizeof(smvk::placement::PlacementVsPush) == 32, "contract.placement-size-32");
	Check(smvk::placement::kPlacementVsPushOffset + sizeof(smvk::placement::PlacementVsPush) <= kScratchEnd,
		"contract.placement-fits-scratch");
	// The mesh tail OVERLAYS the filtered tail's trailing 16B (the VS-skin
	// fields): the mesh legs never execute a VS (their deformed pair rides
	// the GfxMeshParams blob), so the shared bytes are mutually exclusive
	// legs — pin the exact overlay (same start, same end).
	Check(smvk::gfx::kMeshTailPushOffset
		== smvk::gfx::kFilteredVsPushOffset + sizeof(smvk::gfx::FilteredVsPush)
			- sizeof(smvk::gfx::MeshTailPush),
		"contract.meshtail-overlays-filtered-skin-tail");
	// 240B = 2 matrices (128) + 11 BDAs (88) + 4 uints + the deformed BDA
	// (8) + 2 uints. The lightmap BDA sits between shading and indices on
	// BOTH sides (gfxmesh.hlsli) — never reorder one side alone.
	Check(sizeof(smvk::gfx::GfxMeshParams) == 240, "contract.meshparams-240");
	// The filtered MOTION pass's prev-model tail owns the LAST scratch
	// word (240..256 — vsPrevDeformed spans 240..248, which is why the
	// direct legs' per-draw userdata at 244 can never leak into it: the
	// motion pass pushes this tail before its draws, a different
	// pipeline's full-word overlay). Pin the claim (docs/
	// shader_library_design.md Phase A's scratch census).
	Check(smvk::gfx::kFilteredMotionPrevTailOffset == 240,
		"contract.motionprev-offset-240");
	Check(smvk::gfx::kFilteredMotionPrevTailOffset
			+ sizeof(smvk::gfx::FilteredMotionPrevTail) == kScratchEnd,
		"contract.motionprev-ends-scratch");
	// The GPU draw-record ABI (C++ Draw == HLSL GfxDraw, 112B since the
	// userdata word + stride pads landed — the design's documented
	// 96->112 growth; the upload/build/cull math all derive from
	// sizeof, so this pin is the test-side guard of the whole ring).
	Check(sizeof(smvk::gfx::Draw) == 112, "contract.draw-stride-112");
	// The direct legs' per-draw scratch pair: dither flag at 240
	// (padEnd_[0]) + resolved material userdata at 244 (padEnd_[1]).
	Check(offsetof(PC, padEnd_) == 240, "contract.padend-240");
	{
		const PC pcPin{};
		Check(sizeof(pcPin.padEnd_) == 16, "contract.padend-spans-tail");
	}

	// 2. Mesh eligibility truth table.
	Check(smvk::gfx::MeshDrawEligible(smvk::gfx::DrawSetBit_Opaque, 0),
		"contract.mesh-opaque");
	Check(!smvk::gfx::MeshDrawEligible(smvk::gfx::DrawSetBit_Masked, 0),
		"contract.mesh-masked-excluded");
	Check(!smvk::gfx::MeshDrawEligible(0, 0), "contract.mesh-empty-set-excluded");

	// 3. GPU-driven culling is the only path (no kill switch, no legacy
	// CPU regime — the chain always runs; TOURMALINE_GFX_GPU_CULL is an
	// unknown var now).
	{
		engine::RunConfig c = engine::RunConfig::Parse();
		const char* mesh = std::getenv("TOURMALINE_MESH");
		const bool expectMesh = !(mesh && std::string(mesh) == "0");
		Check((!c.meshSet && c.mesh) || (c.mesh == expectMesh),
			"contract.runconfig-mesh-default-on");
	}

	// 4. Filtered/motion tail prefix identity (192..216 shared).
	Check(offsetof(smvk::gfx::FilteredVsPush, draws) == 0, "contract.tail-draws-0");
	Check(offsetof(smvk::gfx::FilteredVsPush, renderViews) == 8,
		"contract.tail-renderviews-8");
	Check(offsetof(smvk::gfx::FilteredVsPush, renderViewIndex) == 16,
		"contract.tail-viewindex-16");
	// lmXforms rides the trailing word (216..224) the motion VS overwrites
	// with its FrameUniforms BDA — same offset, intentional alias.
	Check(offsetof(smvk::gfx::FilteredVsPush, lmXforms) == 24,
		"contract.tail-lmxforms-24");

	// Scratch invariant anchor: shared fields end exactly at 192.
	Check(offsetof(PC, prevModel) == kScratchStart, "contract.scratch-starts-192");
	Check(sizeof(PC) == kScratchEnd, "contract.push-ends-256");

	// 5. The sorter's self-edge contracts.
	TestTopoSortSelfEdges();

	// 6. The (stage x access) pairing contract.
	TestStageAccessContract();

	// 7. The GI volume layer table capacity/pack contract (§1.1).
	TestGiVolumeLayerTableContract();

	std::cout << "checks=" << g_checks << " failures=" << g_failures << "\n";
	return g_failures == 0 ? 0 : 1;
}
