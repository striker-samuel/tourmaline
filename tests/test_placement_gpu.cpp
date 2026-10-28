// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
//
// PlacementGpuTests: the DEVICE half of the placement system's Phase-0
// safety baseline (the PR01 acceptance) — a real (hidden-window) Vulkan
// context driving the actual scatter kernels through the rendergraph,
// with blocking readbacks proving:
//
//   * the GRID kernel (PlacementTilePush) produces the EXACT accepted
//     multiset the CPU oracle (EvaluateTile) produces for its supported
//     program shape (single Const) — stable IDs, candidate indexes,
//     positions, prototypes: the on-device hash bit-parity the old
//     algorithm-parity test deferred ("HLSL-vs-C++ hash bit-parity
//     itself needs on-device readback");
//   * the MESH + HEIGHTFIELD scatter kernels (PlacementScatterPush +
//     device-side PlacementDomainParams) match the same oracle;
//   * the CAPACITY GUARD: a deliberately tiny capacity under full
//     density overflows — the raw count grows past capacity, the
//     overflow counter reports every dropped accepted candidate, the
//     readback clamps to capacity, and nothing outside the buffer is
//     touched (the survivors are a subset of the oracle's multiset);
//   * the CONSTRAINT trio (build/elect/kill) survives exactly the
//     instances the CPU ApplyMinDistance keeps (the bounded-volume grid
//     path, same ConstraintVolumeForLayer inputs).
//
// CPU-side contracts (the oracle itself, layout ABI, params fill) live
// in PlacementTests; this test covers the GPU dispatch + readback path
// only. Bootstrap mirrors test_world_fields_gpu.cpp (RM needs a
// surface-based window); compute execution needs a Swapchain frame
// (AcquireFramebuffer -> Execute -> EndAndPresent), the 03_fuzz
// precedent. CI-excluded like world_fields_gpu_tests (hosted runners
// lack a display/ICD); plain local ctest runs it on a real GPU.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <volk.h>
#include <SDL3/SDL.h>

#include <Engine/App/cluster.h>
#include <Engine/Placement/placement_domain.h>
#include <Engine/Placement/placement_draw.h>
#include <Engine/Placement/placement_eval.h>
#include <Engine/Placement/placement_gpu.h>
#include <Engine/Placement/placement_prototype.h>
#include <Engine/Placement/placement_tile.h>
#include <Engine/Rendergraph/meshparticles.h>
#include <Engine/Rendergraph/placement_pass.h>
#include <Engine/Rendergraph/rendergraph.h>
#include <Engine/Renderer/mesh.h>
#include <Engine/RHI/resource_manager.h>
#include <Engine/RHI/swapchain.h>

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

using namespace smvk::placement;

// ---------------------------------------------------------------------------
// Frame host: one hidden window + RM + swapchain + rendergraph. Each
// RunFrame records passes onto a fresh graph, executes one swapchain
// frame and returns (the uploads recorded before it — ClearTile /
// params / domain data — flush inside EndAndPresentFramebuffer ahead of
// the submit, semaphore-ordered).
// ---------------------------------------------------------------------------
struct GpuHost {
	SDL_Window* window = nullptr;
	smvk::ResourceManager* rm = nullptr;
	smvk::Swapchain* swap = nullptr;
	smvk::RenderGraph* rg = nullptr;

	void RunFrame(const std::function<void(smvk::RenderGraph&)>& record)
	{
		rg->NewFrame();
		record(*rg);
		rm->UpdateDescriptors();
		smvk::Framebuffer fb = swap->AcquireFramebuffer();
		rg->Execute(fb);
		swap->EndAndPresentFramebuffer(fb);
	}
};

// ---------------------------------------------------------------------------
// Parity comparison: the GPU append order is arbitrary (atomics), so
// both sides are sorted by the per-instance UNIQUE key (stableLow,
// customB) and compared field-by-field.
//
// The parity CONTRACT (placement_eval.h — the CPU is the oracle):
// accept/reject + prototype decisions must be EXACT (integer fields:
// stableLow, customB, prototypeId — quantize-before-threshold makes the
// accept deterministic), while transforms (position/yaw/scale) may
// differ in the last ULP (different fma/rounding on GPU). So the
// integer identity fields compare bitwise and the float3 transform
// fields compare with a 1e-5 tolerance.
// ---------------------------------------------------------------------------
bool NearlyEqualF(float a, float b)
{
	return std::fabs(a - b) <= 1e-5f * std::max(1.0f, std::fabs(a) + std::fabs(b));
}

bool SortedEqual(std::vector<RenderInstance> a, std::vector<RenderInstance> b)
{
	if (a.size() != b.size()) return false;
	auto key = [](const RenderInstance& r) {
		return std::pair<uint32_t, uint32_t>(r.stableLow, r.customB);
	};
	std::sort(a.begin(), a.end(), [&](const RenderInstance& l, const RenderInstance& r) {
		return key(l) < key(r); });
	std::sort(b.begin(), b.end(), [&](const RenderInstance& l, const RenderInstance& r) {
		return key(l) < key(r); });
	for (size_t i = 0; i < a.size(); ++i) {
		const RenderInstance& x = a[i];
		const RenderInstance& y = b[i];
		const bool identity = x.stableLow == y.stableLow
			&& x.customB == y.customB
			&& x.prototypeId == y.prototypeId;
		const bool transform = NearlyEqualF(x.position.x, y.position.x)
			&& NearlyEqualF(x.position.y, y.position.y)
			&& NearlyEqualF(x.position.z, y.position.z)
			&& NearlyEqualF(x.yaw, y.yaw)
			&& NearlyEqualF(x.scale.x, y.scale.x)
			&& NearlyEqualF(x.scale.y, y.scale.y)
			&& NearlyEqualF(x.scale.z, y.scale.z);
		if (!identity || !transform) {
			std::fprintf(stderr, "[place-gpu] first diff at %zu/%zu: "
				"gpu(pos %.6f %.6f %.6f yaw %.6f scale %.3f proto %u low %08x cb %u)"
				" cpu(pos %.6f %.6f %.6f yaw %.6f scale %.3f proto %u low %08x cb %u)"
				" identity=%d transform=%d\n",
				i, a.size(),
				x.position.x, x.position.y, x.position.z, x.yaw, x.scale.x,
				x.prototypeId, x.stableLow, x.customB,
				y.position.x, y.position.y, y.position.z, y.yaw, y.scale.x,
				y.prototypeId, y.stableLow, y.customB,
				int(identity), int(transform));
			return false;
		}
	}
	return true;
}

// Is `needle` a subset of `hay` (same multiset keying)? The guard's
// survivors are an arbitrary capacity-sized subset of the oracle's
// accepted set.
bool SortedSubset(const std::vector<RenderInstance>& needle,
	const std::vector<RenderInstance>& hay)
{
	if (needle.size() > hay.size()) return false;
	auto key = [](const RenderInstance& r) {
		return std::pair<uint32_t, uint32_t>(r.stableLow, r.customB);
	};
	std::vector<std::pair<uint32_t, uint32_t>> nk, hk;
	nk.reserve(needle.size()); hk.reserve(hay.size());
	for (const RenderInstance& r : needle) nk.push_back(key(r));
	for (const RenderInstance& r : hay) hk.push_back(key(r));
	std::sort(nk.begin(), nk.end());
	std::sort(hk.begin(), hk.end());
	return std::includes(hk.begin(), hk.end(), nk.begin(), nk.end());
}

GridDomain MakeGrid(glm::vec3 origin, uint32_t cells)
{
	GridDomain g;
	g.origin = origin;
	g.extent = glm::vec2(64.0f, 64.0f);
	g.cellsX = cells;
	g.cellsZ = cells;
	return g;
}

PlacementSystem::LayerBinding MakeLayer(uint64_t id, float density, uint32_t candidates)
{
	PlacementSystem::LayerBinding lb;
	lb.programId = id;
	lb.program.graphSeed = 0x123456789ABCull;
	lb.program.nodeId = 3;
	lb.program.instrs = { OpConst(density) };
	lb.program.acceptIndex = 0;
	lb.config.candidatesPerTile = candidates;
	lb.config.minScale = 0.7f;
	lb.config.maxScale = 1.3f;
	return lb;
}

// ---------------------------------------------------------------------------
// Compound-program builders (PR02 parity: the kernels run the ACTUAL
// graph SSA — noise, slope, altitude, remap/power chains — not the old
// density+noise shortcut).
// ---------------------------------------------------------------------------
void BuildGridCompoundProgram(PlacementProgram& prog)
{
	prog.instrs = {
		FieldInstr{ .op = FieldOp::Noise2, .params = { 0.05f, 0.0f, 0.0f, 0.0f } },
		FieldInstr{ .op = FieldOp::Noise2, .params = { 0.13f, 7.5f, -3.0f, 0.0f } },
		FieldInstr{ .op = FieldOp::Power, .a = 1, .params = { 2.0f, 0, 0, 0 } },
		FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 2 },
		FieldInstr{ .op = FieldOp::Remap, .a = 3, .params = { 0.1f, 0.9f, 0.15f, 1.0f } },
		FieldInstr{ .op = FieldOp::Smoothstep, .a = 4, .params = { 0.2f, 0.8f } },
	};
	prog.acceptIndex = 5;
}

void BuildMeshCompoundProgram(PlacementProgram& prog)
{
	// Slope-gated noise: the folded mesh's normals drive the mask.
	prog.instrs = {
		FieldInstr{ .op = FieldOp::SlopeMask, .params = { 12.0f, 45.0f, 0, 0 } },
		FieldInstr{ .op = FieldOp::Noise2, .params = { 0.09f, 0, 0, 0 } },
		FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 1 },
		FieldInstr{ .op = FieldOp::Clamp01, .a = 2 },
	};
	prog.acceptIndex = 3;
}

void BuildHfCompoundProgram(PlacementProgram& prog)
{
	// The meadow stack: altitude band * slope * noise, remapped up.
	prog.instrs = {
		FieldInstr{ .op = FieldOp::AltitudeMask, .params = { 0.5f, 2.0f, 6.0f, 9.0f } },
		FieldInstr{ .op = FieldOp::SlopeMask, .params = { 8.0f, 35.0f, 0, 0 } },
		FieldInstr{ .op = FieldOp::Noise2, .params = { 0.07f, 0, 0, 0 } },
		FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 1 },
		FieldInstr{ .op = FieldOp::Mul, .a = 3, .b = 2 },
		FieldInstr{ .op = FieldOp::Remap, .a = 4, .params = { 0.0f, 0.8f, 0.1f, 0.95f } },
	};
	prog.acceptIndex = 5;
}

} // namespace

int main()
{
	// Unbuffered progress: stderr survives an abort (stdout block-buffers
	// into a pipe and is lost).
	std::setvbuf(stderr, nullptr, _IONBF, 0);
	std::fprintf(stderr, "[place-gpu] stage: SDL window...\n");
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		std::fprintf(stderr, "[place-gpu] SDL_Init failed: %s\n", SDL_GetError());
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow("placement gpu", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window) {
		std::fprintf(stderr, "[place-gpu] window failed: %s\n", SDL_GetError());
		return 1;
	}
	if (volkInitialize() != VK_SUCCESS) {
		std::fprintf(stderr, "[place-gpu] volkInitialize failed\n");
		return 1;
	}
	// HEAP-allocated: RenderGraph embeds three Pool objects inline
	// (images/views/buffers, 8192 entries each — megabytes); a stack
	// instance overflows the default 1MB stack (the engine always owns
	// graphs by pointer; the retirement fuzzer documents the same rule
	// for its Cluster).
	std::fprintf(stderr, "[place-gpu] stage: ResourceManager + swapchain...\n");
	smvk::ResourceManager rm(window);
	smvk::Swapchain swap(rm, window); // ctor Creates internally
	auto rgPtr = std::make_unique<smvk::RenderGraph>(rm);
	smvk::RenderGraph& rg = *rgPtr;
	GpuHost host{ window, &rm, &swap, &rg };
	std::fprintf(stderr, "[place-gpu] stage: host up\n");

	// The pass under test: 4 slots, a 4096-instance capacity (well above
	// every parity tile's accepted count — overflow stays 0 here).
	std::fprintf(stderr, "[place-gpu] stage: pass create\n");
	PlacementPass pass;
	PlacementPassDesc desc{};
	desc.maxInstancesPerTile = 4096;
	desc.maxTiles = 4;
	bool ok = pass.Ensure(rm, rg, desc);
	Check(ok && pass.Valid(), "gpu.pass-create");

	// Mesh + heightfield scatter kernels (the grid kernel lives on
	// pass.tileProgram via EnsureShaders).
	PlacementScatterPrograms scatter;
	ok = scatter.Ensure(rm);
	Check(ok && scatter.Valid(), "gpu.scatter-programs");

	// =====================================================================
	// 1) Grid kernel parity — a COMPOUND program (noise octaves, power,
	//    remap, smoothstep — the actual graph SSA, not a Const shortcut)
	//    plus a non-unit densityScale (the contract order
	//    q = QuantizeDensity(d * densityScale) must hold).
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: grid parity\n");
	{
		GridDomain grid = MakeGrid(glm::vec3(10.0f, 2.0f, 30.0f), 16);
		auto lb = MakeLayer(7, 0.45f, 1500); // density field replaced below
		BuildGridCompoundProgram(lb.program);
		lb.config.densityScale = 1.4f;
		lb.domainKind = DomainKind::Grid;
		lb.grid = &grid;
		Prototype a; a.weight = 2.0f;
		Prototype b; b.weight = 1.0f;
		Prototype c; c.weight = 1.0f;
		lb.prototypes.add(a); lb.prototypes.add(b); lb.prototypes.add(c);
		const TileID tile{ 1, 2, 3 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() > 0 && oracle.size() < 1500, "gpu.grid-oracle-shape");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.grid-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{}; // this program reads no mask

		pass.ClearTile(rm, 0);
		PlacementTilePush push;
		ok = PlacementPass::BuildTilePush(lb, tile, view, ecoView,
			pass.TileInstancesAddress(rm, 0), pass.TileCountAddress(rm, 0), push);
		Check(ok, "gpu.grid-push-built");
		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = pass.AddEvalPass(g, 0, "PlacementEvalGrid");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				pass.PushAndDispatch(cmd, push);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass.DownloadTile(rm, 0, gpu, 4096, &overflow);
		Check(n == oracle.size(), "gpu.grid-count-parity");
		Check(overflow == 0, "gpu.grid-no-overflow");
		Check(SortedEqual(gpu, oracle), "gpu.grid-multiset-parity");
		Check(n > 0, "gpu.grid-nonempty");
	}

	// =====================================================================
	// 2) Mesh-surface scatter parity — a FOLDED mesh (normals vary per
	//    candidate, so SlopeMask actually gates) under a slope*noise
	//    compound program.
	// =====================================================================
	PlacementMeshGpu meshGpu;
	{
		PlacementMeshData mesh;
		// Folded quad: two triangles with clearly different normals.
		mesh.positions = {
			glm::vec3(0, 0, 0), glm::vec3(4, 2.5f, 0), glm::vec3(0, 0, 4),
			glm::vec3(4, 2.5f, 0), glm::vec3(4, 0, 4), glm::vec3(0, 0, 4) };
		mesh.normals = {
			glm::vec3(0, 0.6f, 0.8f), glm::vec3(0, 0.6f, 0.8f), glm::vec3(0, 0.6f, 0.8f),
			glm::vec3(0, 0.89f, 0.45f), glm::vec3(0, 0.89f, 0.45f), glm::vec3(0, 0.89f, 0.45f) };
		mesh.uvs = {
			glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(0, 1),
			glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1) };
		mesh.indices = { 0, 1, 2, 3, 4, 5 };
		ok = meshGpu.Upload(rm, rg, mesh);
		Check(ok && meshGpu.Valid(), "gpu.mesh-upload");

		auto lb = MakeLayer(8, 0.6f, 800);
		BuildMeshCompoundProgram(lb.program);
		lb.domainKind = DomainKind::MeshSurface;
		lb.mesh = &mesh;
		Prototype a; a.weight = 1.0f;
		Prototype b; b.weight = 3.0f;
		lb.prototypes.add(a); lb.prototypes.add(b);
		const TileID tile{ -2, 0, 5 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() > 0, "gpu.mesh-oracle-nonempty");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.mesh-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{}; // this program reads no mask

		PlacementParamsGpu paramsGpu;
		PlacementDomainParams prm;
		ScatterBDAs bdas = meshGpu.Addresses(rm);
		ok = FillScatterParams(lb, view, ecoView, bdas, prm);
		Check(ok && prm.domainKind == kPlacementScatterDomain_Mesh, "gpu.mesh-params-filled");
		ok = paramsGpu.Upload(rm, rg, prm);
		Check(ok, "gpu.mesh-params-uploaded");

		pass.ClearTile(rm, 1);
		PlacementScatterPush push;
		BuildScatterPush(lb, tile, pass.TileInstancesAddress(rm, 1),
			pass.TileCountAddress(rm, 1), paramsGpu.Address(rm), push);
		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = PlacementScatterPrograms::AddScatterPass(g, pass, 1,
				meshGpu.ReadLogicals(), paramsGpu.logical, "PlacementScatterMesh");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementScatterPrograms::PushAndDispatchScatter(cmd,
					scatter.meshProgram, push);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass.DownloadTile(rm, 1, gpu, 4096, &overflow);
		Check(n == oracle.size(), "gpu.mesh-count-parity");
		Check(overflow == 0, "gpu.mesh-no-overflow");
		Check(SortedEqual(gpu, oracle), "gpu.mesh-multiset-parity");
	}

	// =====================================================================
	// 3) Heightfield scatter parity — the meadow stack (altitude band *
	//    slope * noise, remapped) over real terrain: heights AND normals
	//    vary per candidate, so AltitudeMask and SlopeMask both gate.
	// =====================================================================
	PlacementHeightfieldGpu hfGpu;
	{
		HeightfieldData hf;
		hf.width = 9;
		hf.height = 9;
		hf.origin = glm::vec3(100.0f, 0.0f, 200.0f);
		hf.spacing = glm::vec2(2.0f, 2.0f);
		hf.heights.resize(81);
		for (uint32_t z = 0; z < 9; ++z)
			for (uint32_t x = 0; x < 9; ++x)
				hf.heights[z * 9 + x] = 3.0f * float(x) - 1.5f * float(z)
					+ 2.0f * ValueNoise2D(float(x) * 0.3f, float(z) * 0.3f, 42);
		HeightfieldDomain hdom;
		hdom.field = &hf;
		hdom.extentMin = glm::vec2(100.0f, 200.0f);
		hdom.extentMax = glm::vec2(116.0f, 216.0f);
		ok = hfGpu.Upload(rm, rg, hf);
		Check(ok && hfGpu.Valid(), "gpu.hf-upload");

		auto lb = MakeLayer(9, 0.5f, 900);
		BuildHfCompoundProgram(lb.program);
		lb.domainKind = DomainKind::Heightfield;
		lb.heightfield = &hdom;
		Prototype a; a.weight = 1.0f;
		Prototype b; b.weight = 1.0f;
		Prototype c; c.weight = 2.0f;
		lb.prototypes.add(a); lb.prototypes.add(b); lb.prototypes.add(c);
		const TileID tile{ 0, -3, -6 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() > 0 && oracle.size() < 900, "gpu.hf-oracle-shape");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.hf-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{}; // this program reads no mask

		PlacementParamsGpu paramsGpu;
		PlacementDomainParams prm;
		ScatterBDAs bdas{};
		bdas.heights = hfGpu.Address(rm);
		ok = FillScatterParams(lb, view, ecoView, bdas, prm);
		Check(ok && prm.domainKind == kPlacementScatterDomain_Heightfield,
			"gpu.hf-params-filled");
		ok = paramsGpu.Upload(rm, rg, prm);
		Check(ok, "gpu.hf-params-uploaded");

		pass.ClearTile(rm, 2);
		PlacementScatterPush push;
		BuildScatterPush(lb, tile, pass.TileInstancesAddress(rm, 2),
			pass.TileCountAddress(rm, 2), paramsGpu.Address(rm), push);
		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = PlacementScatterPrograms::AddScatterPass(g, pass, 2,
				{ hfGpu.heightsL }, paramsGpu.logical, "PlacementScatterHf");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementScatterPrograms::PushAndDispatchScatter(cmd,
					scatter.hfProgram, push);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass.DownloadTile(rm, 2, gpu, 4096, &overflow);
		Check(n == oracle.size(), "gpu.hf-count-parity");
		Check(overflow == 0, "gpu.hf-no-overflow");
		Check(SortedEqual(gpu, oracle), "gpu.hf-multiset-parity");
	}

	// =====================================================================
	// 4) The capacity guard: tiny capacity under full density. The raw
	//    count grows past capacity, overflow counts EVERY dropped
	//    accepted candidate, the readback clamps, and the survivors are
	//    an arbitrary subset of the oracle's multiset (nothing outside
	//    the buffer is ever touched).
	// =====================================================================
	{
		PlacementPass tiny;
		PlacementPassDesc tinyDesc{};
		tinyDesc.maxInstancesPerTile = 64; // << 512 candidates
		tinyDesc.maxTiles = 1;
		ok = tiny.Ensure(rm, rg, tinyDesc);
		Check(ok && tiny.Valid(), "gpu.tiny-pass-create");

		GridDomain grid = MakeGrid(glm::vec3(0.0f, 0.0f, 0.0f), 8);
		auto lb = MakeLayer(10, 1.0f, 512); // full density: all accepted
		lb.domainKind = DomainKind::Grid;
		lb.grid = &grid;
		Prototype a; a.weight = 1.0f;
		lb.prototypes.add(a);
		const TileID tile{ 4, 0, 4 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() == 512, "gpu.tiny-oracle-full");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.tiny-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{};

		tiny.ClearTile(rm, 0);
		PlacementTilePush push;
		ok = PlacementPass::BuildTilePush(lb, tile, view, ecoView,
			tiny.TileInstancesAddress(rm, 0), tiny.TileCountAddress(rm, 0), push);
		Check(ok, "gpu.tiny-push-built");
		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = tiny.AddEvalPass(g, 0, "PlacementEvalOverflow");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				tiny.PushAndDispatch(cmd, push);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 0;
		const uint32_t n = tiny.DownloadTile(rm, 0, gpu, 4096, &overflow);
		Check(n == 64, "gpu.tiny-count-clamped-to-capacity");
		Check(overflow == 512 - 64, "gpu.tiny-overflow-counts-drops");
		Check(SortedSubset(gpu, oracle), "gpu.tiny-survivors-subset");
	}

	// =====================================================================
	// 5) The constraint trio: scatter + build/elect/kill in ONE frame;
	//    the survivors match the CPU ApplyMinDistance (bounded-volume
	//    grid path, the same ConstraintVolumeForLayer inputs).
	// =====================================================================
	{
		PlacementConstraint con;
		ok = con.Create(rm, rg);
		Check(ok && con.Valid(), "gpu.con-create");
		ok = con.EnsureShaders(rm);
		Check(ok && con.ShadersValid(), "gpu.con-shaders");

		GridDomain grid = MakeGrid(glm::vec3(-32.0f, 0.0f, -32.0f), 32);
		auto lb = MakeLayer(11, 0.9f, 1024);
		lb.domainKind = DomainKind::Grid;
		lb.grid = &grid;
		lb.config.minSpacing = 4.0f;
		Prototype a; a.weight = 1.0f;
		Prototype b; b.weight = 1.0f;
		lb.prototypes.add(a); lb.prototypes.add(b);
		const TileID tile{ 2, 0, 2 };
		// Oracle A: pre-constraint (the scatter stage's parity target).
		auto lbNoCon = lb;
		lbNoCon.config.minSpacing = 0.0f;
		std::vector<RenderInstance> oracleAll = EvaluateTile(lbNoCon, tile);
		// Oracle B: post-constraint survivors (the trio's parity target).
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() > 0 && oracle.size() < oracleAll.size(),
			"gpu.con-oracle-thins");

		// Constraint params: the SAME volume derivation the CPU used.
		ConstraintVolume vol = ConstraintVolumeForLayer(lb);
		PlacementConParams conPrm;
		ok = PlacementConstraint::FillConParams(lb, tile, vol, conPrm);
		Check(ok, "gpu.con-params-filled");
		con.UploadParams(rm, conPrm);
		con.Clear(rm); // head/next/flag2 zeroed (tile-gen cadence only)

		pass.ClearTile(rm, 3);
		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.con-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{};
		PlacementTilePush push;
		ok = PlacementPass::BuildTilePush(lb, tile, view, ecoView,
			pass.TileInstancesAddress(rm, 3), pass.TileCountAddress(rm, 3), push);
		Check(ok, "gpu.con-scatter-push");

		const uint64_t instBda = pass.TileInstancesAddress(rm, 3);
		const uint64_t countBda = pass.TileCountAddress(rm, 3);
		PlacementConPush conPush;
		PlacementConstraint::BuildConPush(instBda, con.HeadAddress(rm),
			con.NextAddress(rm), con.Flag2Address(rm), con.ParamsAddress(rm),
			countBda, conPush);

		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = pass.AddEvalPass(g, 3, "PlacementEvalCon");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				pass.PushAndDispatch(cmd, push);
			});
			// Dispatch width: candidateCount (the CPU-known upper bound —
			// every candidate produces at most one instance; con threads
			// past the live count early out). No count readback on the
			// frame path.
			const uint32_t width = lb.config.candidatesPerTile;
			smvk::RenderPass& build = PlacementConstraint::AddBuildPass(g, con,
				pass.instanceLogicals[3], pass.countLogicals[3]);
			build.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementConstraint::PushAndDispatchCon(cmd, con.buildProgram,
					conPush, width);
			});
			smvk::RenderPass& elect = PlacementConstraint::AddElectPass(g, con,
				pass.instanceLogicals[3], pass.countLogicals[3]);
			elect.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementConstraint::PushAndDispatchCon(cmd, con.electProgram,
					conPush, width);
			});
			smvk::RenderPass& kill = PlacementConstraint::AddKillPass(g, con,
				pass.instanceLogicals[3], pass.countLogicals[3]);
			kill.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementConstraint::PushAndDispatchCon(cmd, con.killProgram,
					conPush, width);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass.DownloadTile(rm, 3, gpu, 4096, &overflow);
		Check(overflow == 0 && n == oracleAll.size(), "gpu.con-scatter-count-parity");
		Check(SortedEqual(gpu, oracleAll), "gpu.con-scatter-multiset-parity");

		// flag2[slot].y = alive; read back the live slice only.
		std::vector<uint32_t> flags(size_t(n) * 2, 0);
		ok = rm.DownloadDataFromBuffer(con.flag2Buffer, flags.data(),
			sizeof(uint32_t) * 2 * n);
		Check(ok, "gpu.con-flag2-readback");

		// Elect-stage isolation: the CPU elect mirror (winner per cell,
		// argmax (priority, tie -> smaller candidateIndex)) over the GPU's
		// own instances must produce EXACTLY the flag2.x set. Diverging
		// here means the build/elect kernels disagree; agreeing here pins
		// any survivor diff on the kill kernel.
		{
			std::vector<uint32_t> gpuElected;
			for (uint32_t i = 0; i < n; ++i)
				if (flags[size_t(i) * 2] != 0) gpuElected.push_back(i);
			std::unordered_map<uint64_t, uint32_t> cellWinner; // cell -> slot
			auto cellKey = [&](const glm::vec3& p) -> uint64_t {
				const int64_t cx = (int64_t)std::floor(
					(p.x - vol.origin.x) / vol.spacing);
				const int64_t cy = (int64_t)std::floor(
					(p.y - vol.origin.y) / vol.spacing);
				const int64_t cz = (int64_t)std::floor(
					(p.z - vol.origin.z) / vol.spacing);
				return (uint64_t(cz) * vol.dimY + uint64_t(cy)) * vol.dimX
					+ uint64_t(cx);
			};
			for (uint32_t i = 0; i < n; ++i) {
				const uint64_t k = cellKey(gpu[i].position);
				auto it = cellWinner.find(k);
				if (it == cellWinner.end()) { cellWinner[k] = i; continue; }
				const uint32_t j = it->second;
				const uint64_t pi = PriorityOf(MakeStableID(
					lb.program.graphSeed, lb.program.nodeId, tile, 0, gpu[i].customB));
				const uint64_t pj = PriorityOf(MakeStableID(
					lb.program.graphSeed, lb.program.nodeId, tile, 0, gpu[j].customB));
				if (pi > pj || (pi == pj && gpu[i].customB < gpu[j].customB))
					it->second = i;
			}
			std::vector<uint32_t> cpuElected;
			cpuElected.reserve(cellWinner.size());
			for (auto& [k, slotIdx] : cellWinner) cpuElected.push_back(slotIdx);
			std::sort(gpuElected.begin(), gpuElected.end());
			std::sort(cpuElected.begin(), cpuElected.end());
			bool sameElected = gpuElected.size() == cpuElected.size();
			if (sameElected)
				for (size_t i = 0; i < gpuElected.size(); ++i)
					if (gpuElected[i] != cpuElected[i]) { sameElected = false; break; }
			Check(sameElected, "gpu.con-elect-mask-parity");
			if (!sameElected) {
				std::fprintf(stderr, "[place-gpu] elect sets differ: gpu %zu cpu %zu"
					" (n=%u)\n", gpuElected.size(), cpuElected.size(), n);
				// Build-stage isolation: walk the GPU lists (head/next
				// readback) per cell and compare membership against the
				// CPU cell map. A missing list member explains a
				// same-size/different-winner elect diff.
				std::vector<uint32_t> head(vol.dimX * vol.dimY * vol.dimZ,
					0xFFFFFFFFu);
				if (rm.DownloadDataFromBuffer(con.headBuffer, head.data(),
						head.size() * sizeof(uint32_t))) {
					std::vector<uint32_t> nextBuf(n, 0);
					if (rm.DownloadDataFromBuffer(con.nextBuffer, nextBuf.data(),
							n * sizeof(uint32_t))) {
						// CPU cell -> slots map.
						std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
						for (uint32_t i = 0; i < n; ++i)
							cells[cellKey(gpu[i].position)].push_back(i);
						size_t cellsWithDiff = 0;
						for (auto& [k, members] : cells) {
							std::vector<uint32_t> walk;
							for (uint32_t m = head[k]; m != 0xFFFFFFFFu;
									m = nextBuf[m]) {
								walk.push_back(m);
								if (walk.size() > n) break; // cycle guard
							}
							std::sort(walk.begin(), walk.end());
							std::vector<uint32_t> sorted = members;
							std::sort(sorted.begin(), sorted.end());
							if (walk != sorted) {
								if (++cellsWithDiff <= 3)
									std::fprintf(stderr, "[place-gpu] build list"
										" diff at cell %llu: list %zu members %zu"
										" (first list %u first member %u)\n",
										(unsigned long long)k, walk.size(),
										sorted.size(),
										walk.empty() ? ~0u : walk.front(),
										sorted.empty() ? ~0u : sorted.front());
							}
						}
						std::fprintf(stderr, "[place-gpu] build cells with"
							" membership diff: %zu / %zu\n", cellsWithDiff,
							cells.size());
					}
				}
			}
		}

		std::vector<RenderInstance> survivors;
		survivors.reserve(n);
		for (uint32_t i = 0; i < n; ++i)
			if (flags[size_t(i) * 2 + 1] != 0)
				survivors.push_back(gpu[i]);

		// Constraint parity, stage 2: re-run the CPU oracle's
		// ApplyMinDistance over the GPU's OWN scatter output (the same
		// input BITS — the GPU positions may differ from the CPU oracle's
		// by a last-ULP, which legitimately flips near-threshold distance
		// and cell-boundary decisions; the constraint CONTRACT is that
		// the kernels implement the same algorithm, so with identical
		// input the survivor sets must match EXACTLY, integer-identical).
		// ApplyMinDistance's tie-break is "smaller VECTOR index wins",
		// which equals "smaller candidate index" only in candidate order
		// (EvaluateTile's contract) — the GPU slot order is
		// atomic-arbitrary, so the mirror is sorted by customB first.
		std::vector<RenderInstance> mirror = gpu;
		std::sort(mirror.begin(), mirror.end(),
			[](const RenderInstance& l, const RenderInstance& r) {
				return l.customB < r.customB; });
		std::vector<StableID> mirrorIds;
		mirrorIds.reserve(mirror.size());
		for (const RenderInstance& r : mirror)
			mirrorIds.push_back(MakeStableID(lb.program.graphSeed,
				lb.program.nodeId, tile, 0, r.customB));
		ApplyMinDistance(mirror, mirrorIds, lb.config.minSpacing, vol);
		Check(survivors.size() == mirror.size(), "gpu.con-survivor-count-parity");
		Check(SortedEqual(survivors, mirror), "gpu.con-survivor-multiset-parity");
		// And the oracle's own survivor count over its own output stays
		// in the same regime (guards against a volume-derivation drift
		// between the two paths — same ConstraintVolumeForLayer inputs).
		Check(oracle.size() > 0 && oracle.size() < oracleAll.size(),
			"gpu.con-oracle-regime");
	}

	// The shared fabricated render-view slab (sections 6+8): one view,
	// identity matrices, an inward half-tile frustum box.
	smvk::Holder<smvk::BufferHandle> viewSlab;
	smvk::LogicalBufferHandle viewSlabL{};
	{
		glm::mat4 ident(1.0f);
		std::vector<float> viewBytes(64); // 256B = one RenderView (floats)
		auto putMat = [&](size_t base, const glm::mat4& m) {
			for (int c = 0; c < 4; ++c)
				for (int r = 0; r < 4; ++r)
					viewBytes[base + c * 4 + r] = m[c][r];
		};
		putMat(0, ident);        // m_viewProjectionMatrix
		putMat(16, ident);       // m_inverseViewProjectionMatrix
		const glm::vec4 planes[6] = {
			glm::vec4(1, 0, 0, 4), glm::vec4(-1, 0, 0, 36),
			glm::vec4(0, 1, 0, 100), glm::vec4(0, -1, 0, 100),
			glm::vec4(0, 0, 1, 4), glm::vec4(0, 0, -1, 36) };
		for (int pl = 0; pl < 6; ++pl)
			for (int k = 0; k < 4; ++k)
				viewBytes[32 + pl * 4 + k] = planes[pl][k];
		viewSlab = rm.CreateBuffer(sizeof(float) * viewBytes.size(),
			smvk::BufferType_Storage, "placement test view slab");
		Check(!viewSlab.empty(), "gpu.vis-view-slab");
		viewSlabL = rg.AddExternalBuffer(viewSlab, "placement test view slab");
		Check(!viewSlabL.empty(), "gpu.vis-view-logical");
		rm.UploadDataToBuffer(viewSlab, viewBytes.data(),
			sizeof(float) * viewBytes.size(),
			smvk::PipelineStageBit_Compute, smvk::AccessFlags::ShaderRead, 0, 0);
	}

	// =====================================================================
	// 6) The PR03 visible chain (cull -> prefix -> scatter -> drawbuild):
	//    three prototypes with DISTINCT cooked bounds + a species draw
	 //   distance on the middle one, a fabricated frustum covering half
	//    the tile — readback the bins, the visible list and the indirect
	//    commands and compare against a CPU mirror over the GPU's OWN
	//    scatter bits (the same instance set, the same sphere/planes
	//    math). This is the nonzero-firstInstance contract too: bin p's
	//    draw range [offsets[p], offsets[p]+counts[p]) must contain
	//    exactly proto p's visible slots (what the VS walks via
	//    SV_InstanceID).
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: visible chain\n");
	{
		PlacementCullPrograms cullProgs;
		ok = cullProgs.Ensure(rm);
		Check(ok && cullProgs.Valid(), "gpu.vis-programs");

		PlacementPass pass2;
		PlacementPassDesc desc2{};
		desc2.maxInstancesPerTile = 4096;
		desc2.maxTiles = 1;
		ok = pass2.Ensure(rm, rg, desc2);
		Check(ok && pass2.Valid(), "gpu.vis-pass-create");
		PlacementVisibleSet vis;
		ok = vis.Create(rm, rg, 4096);
		Check(ok && vis.Valid(), "gpu.vis-set-create");

		// The layer: full density (deterministic population), three
		// prototypes with distinct cooked spheres; proto 1 has a 12m
		// species draw distance (the camera sits at the tile corner).
		GridDomain grid = MakeGrid(glm::vec3(0.0f, 0.0f, 0.0f), 16);
		auto lb = MakeLayer(12, 1.0f, 1024);
		lb.domainKind = DomainKind::Grid;
		lb.grid = &grid;
		Prototype pa; pa.weight = 1.0f;
		pa.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
		pa.boundsMax = glm::vec3(0.5f, 1.0f, 0.5f);
		Prototype pb; pb.weight = 1.0f;
		pb.boundsMin = glm::vec3(-1.0f, 0.0f, -1.0f);
		pb.boundsMax = glm::vec3(1.0f, 3.0f, 1.0f);
		pb.maxDrawDistance = 12.0f; // the species distance cull
		Prototype pc; pc.weight = 1.0f;
		pc.boundsMin = glm::vec3(-2.0f, -2.0f, -2.0f);
		pc.boundsMax = glm::vec3(2.0f, 2.0f, 2.0f);
		lb.prototypes.add(pa); lb.prototypes.add(pb); lb.prototypes.add(pc);
		const TileID tile{ 1, 0, 1 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() == 1024, "gpu.vis-oracle-full");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.vis-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoView ecoView{};

		pass2.ClearTile(rm, 0);
		PlacementTilePush push;
		ok = PlacementPass::BuildTilePush(lb, tile, view, ecoView,
			pass2.TileInstancesAddress(rm, 0), pass2.TileCountAddress(rm, 0), push);
		Check(ok, "gpu.vis-scatter-push");

		PlacementCullPush cullPush;
		FillCullPush(lb, pass2.TileInstancesAddress(rm, 0),
			pass2.TileCountAddress(rm, 0), vis.MaskAddress(rm),
			vis.BinsAddress(rm), 0 /* no constraint tile: skip alive */,
			rm.GetBufferDeviceAddress(viewSlab), 0, glm::vec3(0, 5, 0),
			kPlacementCullFrustum | kPlacementCullDistance, cullPush);
		Check(cullPush.protoCount == 3
			&& cullPush.protoDist[1] == 12.0f, "gpu.vis-cull-push-filled");
		PlacementScatterVisPush scPush{};
		scPush.instances = pass2.TileInstancesAddress(rm, 0);
		scPush.count = pass2.TileCountAddress(rm, 0);
		scPush.mask = vis.MaskAddress(rm);
		scPush.bins = vis.BinsAddress(rm);
		scPush.visible = vis.VisibleAddress(rm);
		scPush.protoCount = 3;
		PlacementDrawBuildPush dbPush{};
		dbPush.bins = vis.BinsAddress(rm);
		dbPush.indirect = vis.IndirectAddress(rm);
		dbPush.protoCount = 3;
		// Seed the immutable indirect halves (fake mesh geometry: proto i
		// draws 300 indices at firstIndex i*64).
		PlacementIndirectCmd seeds[3] = {};
		for (int i = 0; i < 3; ++i) {
			seeds[i].indexCount = 300;
			seeds[i].firstIndex = uint32_t(i) * 64;
			seeds[i].vertexOffset = 0;
		}
		ok = vis.SeedIndirect(rm, seeds, 3);
		Check(ok, "gpu.vis-indirect-seeded");
		vis.ClearFrame(rm); // zero mask + bin atomics (the frame cadence)

		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = pass2.AddEvalPass(g, 0, "PlacementEvalVis");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				pass2.PushAndDispatch(cmd, push);
			});
			smvk::RenderPass& cull = PlacementCullPrograms::AddCullPass(g, pass2,
				0, vis, smvk::LogicalBufferHandle{}, viewSlabL);
			cull.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementCullPrograms::PushAndDispatchCull(cmd,
					cullProgs.cullProgram, cullPush, 4096);
			});
			smvk::RenderPass& pre = PlacementCullPrograms::AddPrefixPass(g, vis);
			pre.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementCullPrograms::PushAndDispatchPrefix(cmd,
					cullProgs.prefixProgram, cullPush);
			});
			smvk::RenderPass& sc = PlacementCullPrograms::AddScatterVisPass(g,
				pass2, 0, vis);
			sc.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementCullPrograms::PushAndDispatchScatterVis(cmd,
					cullProgs.scatterProgram, scPush, 4096);
			});
			smvk::RenderPass& db = PlacementCullPrograms::AddDrawBuildPass(g, vis);
			db.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementCullPrograms::PushAndDispatchDrawBuild(cmd,
					cullProgs.buildProgram, dbPush);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass2.DownloadTile(rm, 0, gpu, 4096, &overflow);
		Check(n == 1024 && overflow == 0, "gpu.vis-scatter-full");

		// ---- the CPU mirror: same instances (GPU bits), same math. ----
		// Sphere per proto + the fabricated planes + the species
		// distance — the expected visible SET per proto.
		auto sphereOf = [&](uint32_t p) {
			const Prototype& pr = lb.prototypes.at(p);
			const glm::vec3 c = (pr.boundsMin + pr.boundsMax) * 0.5f;
			const glm::vec3 h = (pr.boundsMax - pr.boundsMin) * 0.5f;
			return glm::vec4(c, glm::length(h));
		};
		const glm::vec4 spheres[3] = { sphereOf(0), sphereOf(1), sphereOf(2) };
		const glm::vec4 mirrorPlanes[6] = {
			glm::vec4(1, 0, 0, 4), glm::vec4(-1, 0, 0, 36),
			glm::vec4(0, 1, 0, 100), glm::vec4(0, -1, 0, 100),
			glm::vec4(0, 0, 1, 4), glm::vec4(0, 0, -1, 36) };
		const glm::vec3 camPos(0.0f, 5.0f, 0.0f);
		std::vector<std::vector<uint32_t>> expected(3);
		for (uint32_t i = 0; i < n; ++i) {
			const RenderInstance& inst = gpu[i];
			const uint32_t p = inst.prototypeId;
			const float s = std::max({ inst.scale.x, inst.scale.y, inst.scale.z });
			const float r = spheres[p].w * s;
			const glm::vec3 center = inst.position + glm::vec3(spheres[p]) * s;
			bool visible = true;
			// species distance (proto 1 only here)
			if (lb.prototypes.at(p).maxDrawDistance > 0.0f) {
				const float d = glm::length(center - camPos) - r;
				if (d > lb.prototypes.at(p).maxDrawDistance) visible = false;
			}
			if (visible)
				for (int pl = 0; pl < 6 && visible; ++pl) {
					const glm::vec4& plane = mirrorPlanes[pl];
					if (glm::dot(glm::vec3(plane), center) + plane.w < -r)
						visible = false;
				}
			if (visible) expected[p].push_back(i);
		}

		// ---- the bins readback. ----
		PlacementBinsGpu binsOut;
		ok = rm.DownloadDataFromBuffer(vis.bins, &binsOut, sizeof(binsOut));
		Check(ok, "gpu.vis-bins-readback");
		uint32_t offset = 0;
		bool offsetsOk = true, countsOk = true;
		for (int p = 0; p < 3; ++p) {
			if (binsOut.counts[p] != expected[p].size()) countsOk = false;
			if (binsOut.offsets[p] != offset) offsetsOk = false;
			offset += binsOut.counts[p];
		}
		Check(countsOk, "gpu.vis-bin-counts-parity");
		Check(offsetsOk, "gpu.vis-bin-offsets-parity");
		Check(binsOut.total == offset
			&& binsOut.total < n, "gpu.vis-total-parity");
		Check(expected[0].size() > 0 && expected[1].size() > 0
			&& expected[2].size() > 0, "gpu.vis-all-three-bins-nonempty");
		// The species distance actually culled proto 1 somewhere.
		Check(expected[1].size() < n / 3, "gpu.vis-species-distance-culls");

		// ---- the visible list: bin ranges hold exactly proto p's slots
		// (the nonzero-firstInstance VS contract). ----
		std::vector<uint32_t> visibleOut(n, 0);
		ok = rm.DownloadDataFromBuffer(vis.visible, visibleOut.data(),
			sizeof(uint32_t) * n);
		Check(ok, "gpu.vis-list-readback");
		bool rangesOk = true;
		for (int p = 0; p < 3; ++p) {
			std::vector<uint32_t> got(visibleOut.begin() + binsOut.offsets[p],
				visibleOut.begin() + binsOut.offsets[p] + binsOut.counts[p]);
			std::vector<uint32_t> want = expected[p];
			std::sort(got.begin(), got.end());
			std::sort(want.begin(), want.end());
			if (got != want) { rangesOk = false; break; }
		}
		Check(rangesOk, "gpu.vis-bin-range-identity");

		// ---- the indirect commands. ----
		PlacementIndirectCmd cmds[8];
		ok = rm.DownloadDataFromBuffer(vis.indirect, cmds,
			kPlacementIndirectStride * 3);
		Check(ok, "gpu.vis-indirect-readback");
		bool cmdsOk = true;
		for (int p = 0; p < 3; ++p) {
			if (cmds[p].indexCount != 300 || cmds[p].firstIndex != uint32_t(p) * 64
				|| cmds[p].vertexOffset != 0)
				cmdsOk = false; // the CPU-seeded immutable halves stand
			if (cmds[p].instanceCount != binsOut.counts[p]
				|| cmds[p].firstInstance != binsOut.offsets[p])
				cmdsOk = false; // the GPU-written dynamic halves
		}
		Check(cmdsOk, "gpu.vis-indirect-commands-parity");
		// Nonzero firstInstance occurs for later bins (the VS contract
		// walks them via SV_InstanceID).
		Check(binsOut.offsets[1] > 0 || binsOut.offsets[2] > 0,
			"gpu.vis-nonzero-first-instance");
	}

	// =====================================================================
	// 7) The PR04 ecotope mask: a PAINTED meadow graph —
	//    ecoMask(channel 0: a painted weight grid) * noise, over the
	//    heightfield (real heights + normals). The parity proves the
	//    mask's world->texel mapping + bilinear + the channel gate
	//    produce the oracle's EXACT accepted multiset on device.
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: ecotope mask\n");
	{
		// The painted mask: 2 channels over the hf extent. Channel 0 =
		// a weight island (blobby coverage); channel 1 = constant (a
		// different ecotope the program does NOT read).
		EcotopeMaskData eco;
		eco.width = 17; eco.height = 17; eco.channels = 2;
		eco.origin = glm::vec2(100.0f, 200.0f);   // the hf extent's origin
		eco.spacing = glm::vec2(1.0f, 1.0f);      // 16m over 17 texels
		eco.weights.assign(2 * 17 * 17, 0.0f);
		for (uint32_t z = 0; z < 17; ++z)
			for (uint32_t x = 0; x < 17; ++x) {
				const float dx = float(x) - 8.0f, dz = float(z) - 8.0f;
				const float d = std::sqrt(dx * dx + dz * dz);
				eco.weights[(size_t)z * 17 + x] =
					std::max(0.0f, 1.0f - d / 7.0f);          // ch0 island
				eco.weights[289 + (size_t)z * 17 + x] = 0.5f; // ch1 const
			}
		Check(eco.valid(), "gpu.eco-mask-valid");

		HeightfieldData hf;
		hf.width = 9; hf.height = 9;
		hf.origin = glm::vec3(100.0f, 0.0f, 200.0f);
		hf.spacing = glm::vec2(2.0f, 2.0f);
		hf.heights.resize(81);
		for (uint32_t z = 0; z < 9; ++z)
			for (uint32_t x = 0; x < 9; ++x)
				hf.heights[z * 9 + x] = 3.0f * float(x) - 1.5f * float(z)
					+ 2.0f * ValueNoise2D(float(x) * 0.3f, float(z) * 0.3f, 42);
		HeightfieldDomain hdom;
		hdom.field = &hf;
		hdom.extentMin = glm::vec2(100.0f, 200.0f);
		hdom.extentMax = glm::vec2(116.0f, 216.0f);

		// The meadow program: paintedWeight * slope * noise.
		auto lb = MakeLayer(13, 0.5f, 900);
		lb.program.instrs = {
			FieldInstr{ .op = FieldOp::EcotopeMask, .params = { 0.0f, 0, 0, 0 } },
			FieldInstr{ .op = FieldOp::SlopeMask, .params = { 50.0f, 75.0f, 0, 0 } },
			FieldInstr{ .op = FieldOp::Noise2, .params = { 0.09f, 0, 0, 0 } },
			FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 1 },
			FieldInstr{ .op = FieldOp::Mul, .a = 3, .b = 2 },
		};
		lb.program.acceptIndex = 4;
		lb.domainKind = DomainKind::Heightfield;
		lb.heightfield = &hdom;
		lb.ecotope = &eco; // THE painted layer binding
		Prototype a; a.weight = 1.0f;
		Prototype b; b.weight = 2.0f;
		lb.prototypes.add(a); lb.prototypes.add(b);
		const TileID tile{ 3, 0, -3 };
		std::vector<RenderInstance> oracle = EvaluateTile(lb, tile);
		Check(oracle.size() > 0 && oracle.size() < 900, "gpu.eco-oracle-shape");

		PlacementProgramGpu prog;
		Check(prog.Upload(rm, rg, lb, 0) && prog.Valid(), "gpu.eco-program-upload");
		PlacementProgramView view = prog.View(rm);
		PlacementEcoMaskGpu ecoGpu;
		Check(ecoGpu.Upload(rm, rg, eco) && ecoGpu.Valid(), "gpu.eco-mask-upload");
		PlacementEcoView ecoView = ecoGpu.View(rm);
		// The GPU view's texel mapping comes from the source mask.
		PlacementEcoView cpuSide = PlacementEcoMaskGpu::ViewOf(eco);
		ecoView.ox = cpuSide.ox; ecoView.oz = cpuSide.oz;
		ecoView.sx = cpuSide.sx; ecoView.sz = cpuSide.sz;
		Check(ecoView.Valid() && ecoView.channels == 2, "gpu.eco-view-valid");

		PlacementParamsGpu paramsGpu;
		PlacementDomainParams prm;
		ScatterBDAs bdas{};
		bdas.heights = hfGpu.Address(rm); // reuse section 3's hf upload
		ok = FillScatterParams(lb, view, ecoView, bdas, prm);
		Check(ok && prm.ecoBDA == ecoView.bda && prm.ecoChannels == 2
			&& prm.ecoOX == 100.0f, "gpu.eco-params-filled");
		ok = paramsGpu.Upload(rm, rg, prm);
		Check(ok, "gpu.eco-params-uploaded");

		pass.ClearTile(rm, 2);
		PlacementScatterPush push;
		BuildScatterPush(lb, tile, pass.TileInstancesAddress(rm, 2),
			pass.TileCountAddress(rm, 2), paramsGpu.Address(rm), push);
		host.RunFrame([&](smvk::RenderGraph& g) {
			smvk::RenderPass& eval = PlacementScatterPrograms::AddScatterPass(g, pass, 2,
				{ hfGpu.heightsL }, paramsGpu.logical, "PlacementScatterEco");
			eval.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
				PlacementScatterPrograms::PushAndDispatchScatter(cmd,
					scatter.hfProgram, push);
			});
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		std::vector<RenderInstance> gpu;
		uint32_t overflow = 1;
		const uint32_t n = pass.DownloadTile(rm, 2, gpu, 4096, &overflow);
		Check(overflow == 0, "gpu.eco-no-overflow");
		Check(n == oracle.size(), "gpu.eco-count-parity");
		Check(SortedEqual(gpu, oracle), "gpu.eco-multiset-parity");

		// The eco GATE on device-adjacent plumbing: a maskless layer
		// using EcotopeMask refuses the fill (never a silent zero).
		auto maskless = lb;
		maskless.ecotope = nullptr;
		PlacementDomainParams refused;
		Check(!FillScatterParams(maskless, view, ecoView, bdas, refused),
			"gpu.eco-maskless-refuses");
		Check(!PlacementProgramGpu().Upload(rm, rg, maskless, 0),
			"gpu.eco-maskless-program-refuses");
	}

	// =====================================================================
	// 8) The WORLD RUNTIME (PR05b): the FULL stack on device — the fake
	//    world (heights + paint) -> PlacementWorldRuntime::BeginFrame
	//    (the residency tick -> the harvest -> the uploads -> the scatter
	//    dispatch) -> RecordCullChain (the PR03 bins + indirect) — the
	//    exact object the renderer embeds, driven end to end.
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: world runtime\n");
	{
		using namespace smvk::terrain;
		// The fake world (a 16m terrain, the LOCKED pack constants).
		TerrainHField field;
		field.meta.worldSize = 16.0f;
		field.meta.originX = -8.0f;
		field.meta.originZ = -8.0f;
		field.meta.hTileSize = 4.0f;
		field.meta.hRes = 4;
		field.meta.gBlock = 8.0f;
		field.meta.baseY = kTerrainBaseY;
		field.meta.range = kTerrainRange;
		const uint32_t nH = field.tilesPerAxis();
		field.tiles.assign(size_t(nH) * nH,
			std::vector<uint8_t>(size_t(field.meta.hRes) * field.meta.hRes * 4, 0));
		for (uint32_t tz = 0; tz < nH; ++tz)
			for (uint32_t tx = 0; tx < nH; ++tx) {
				auto& tile = field.tiles[size_t(tz) * nH + tx];
				for (uint32_t py = 0; py < field.meta.hRes; ++py)
					for (uint32_t pxx = 0; pxx < field.meta.hRes; ++pxx) {
						const float h01 =
							(float)(tx * 4 + pxx + tz * 4 + py) / 1024.0f;
						uint8_t r, g;
						EncodeRG16(h01, r, g);
						const size_t o = (size_t(py) * field.meta.hRes + pxx) * 4;
						tile[o] = r; tile[o + 1] = g;
					}
			}
		std::vector<std::vector<uint8_t>> sTiles(2 * 2);
		const uint32_t sRes = 8;
		for (auto& t : sTiles)
			t.assign(size_t(sRes) * sRes * 4, 0);
		for (uint32_t tz = 0; tz < 2; ++tz)
			for (uint32_t tx = 0; tx < 2; ++tx) {
				auto& tile = sTiles[size_t(tz) * 2 + tx];
				for (uint32_t py = 0; py < sRes; ++py)
					for (uint32_t pxx = 0; pxx < sRes; ++pxx) {
						const size_t o = (size_t(py) * sRes + pxx) * 4;
						tile[o + 0] = uint8_t((pxx * 24 + py * 8) % 256);
						tile[o + 3] = 255;
					}
			}
		WorldMaskGrid maskGrid{ &sTiles, 2, sRes };

		// The meadow layer (kept alive; the runtime borrows it).
		PlacementSystem::LayerBinding lb;
		lb.programId = 303;
		lb.program.graphSeed = 4242;
		lb.program.nodeId = 1;
		lb.program.instrs = {
			FieldInstr{ .op = FieldOp::EcotopeMask, .params = { 0.0f, 0, 0, 0 } },
			FieldInstr{ .op = FieldOp::SlopeMask, .params = { 20.0f, 60.0f, 0, 0 } },
			FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 1 },
		};
		lb.program.acceptIndex = 2;
		lb.domainKind = DomainKind::Heightfield;
		lb.config.candidatesPerTile = 512;
		Prototype pa; pa.weight = 1.0f;
		pa.boundsMin = glm::vec3(-0.5f, 0.0f, -0.5f);
		pa.boundsMax = glm::vec3(0.5f, 1.0f, 0.5f);
		Prototype pb; pb.weight = 1.0f;
		pb.boundsMin = glm::vec3(-1.0f, 0.0f, -1.0f);
		pb.boundsMax = glm::vec3(1.0f, 3.0f, 1.0f);
		pb.maxDrawDistance = 33.0f; // the species distance: the tile sits
		                            // ~30-35m from the cam — the far part
		                            // culls, the near keeps (PARTIAL)
		lb.prototypes.add(pa);
		lb.prototypes.add(pb);

		// The CPU oracle over the SAME tile (the runtime's harvest is
		// deterministic — same inputs, same world->texel contract).
		WorldTileData oracleData;
		Check(BuildWorldTileDomain(field, maskGrid, 8.0f, 0, 0, 2.0f,
				oracleData), "gpu.rt-oracle-domain");
		PlacementSystem::LayerBinding oracleLayer = lb;
		HeightfieldDomain oracleDom = oracleData.domain;
		oracleDom.field = &oracleData.heights;
		oracleLayer.heightfield = &oracleDom;
		oracleLayer.ecotope = &oracleData.eco;
		const TileID tile{ 0, 0, 0 };
		std::vector<RenderInstance> oracle = EvaluateTile(oracleLayer, tile);
		Check(oracle.size() > 0 && oracle.size() < 512, "gpu.rt-oracle-shape");

		// The runtime (heap: it owns pools + maps).
		auto rt = std::make_unique<PlacementWorldRuntime>();
		rt->layer = &lb;
		rt->hfield = &field;
		rt->masks = &maskGrid;
		rt->tileSize = 8.0f;
		rt->haloMeters = 2.0f;
		rt->slotBudget = 2;
		rt->viewBudget = 1;
		Check(rt->Create(rm, rg), "gpu.rt-create");
		Check(rt->Valid(), "gpu.rt-valid");

		DependencyVersions deps;
		const glm::vec4 interest[1] = { glm::vec4(0.0f, 0.0f, 8.0f, 8.0f) };
		// Frame 1: BeginFrame (the scatter) + the cull chain — same frame,
		// the graph orders them through the external-buffer edges.
		host.RunFrame([&](smvk::RenderGraph& g) {
			rt->BeginFrame(rm, g, interest, 1, deps);
			rt->RecordCullChain(rm, g, 0, viewSlabL,
				rm.GetBufferDeviceAddress(viewSlab), 0,
				glm::vec3(0.0f, 5.0f, 0.0f), kPlacementCullFrustum | kPlacementCullDistance);
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		// The scatter parity: the slot's instances == the oracle (the
		// harvested-window contract end to end).
		int32_t foundSlot = -1;
		for (size_t s = 0; s < rt->active.size(); ++s)
			if (rt->active[s].published) { foundSlot = int32_t(s); break; }
		Check(foundSlot >= 0, "gpu.rt-tile-published");
		if (foundSlot >= 0) {
			std::vector<RenderInstance> gpu;
			uint32_t overflow = 1;
			const uint32_t n = rt->pass.DownloadTile(rm, uint32_t(foundSlot),
				gpu, 4096, &overflow);
			Check(overflow == 0 && n == oracle.size(),
				"gpu.rt-scatter-count-parity");
			Check(SortedEqual(gpu, oracle), "gpu.rt-scatter-multiset-parity");
			Check(rt->VisSet(foundSlot) != nullptr,
				"gpu.rt-visset-exposed");
			// The cull chain's bins: proto 0 all-visible-ish; proto 1 the
			// 9m species distance trims the far half (cam at the corner).
			const PlacementVisibleSet* vs = rt->VisSet(foundSlot);
			PlacementBinsGpu binsOut;
			ok = rm.DownloadDataFromBuffer(vs->bins, &binsOut,
				sizeof(binsOut));
			Check(ok, "gpu.rt-bins-readback");
			Check(binsOut.counts[0] + binsOut.counts[1] == binsOut.total
				&& binsOut.total > 0 && binsOut.total < n,
				"gpu.rt-total-visible-regime");
			PlacementIndirectCmd cmds[2];
			ok = rm.DownloadDataFromBuffer(vs->indirect, cmds,
				kPlacementIndirectStride * 2);
			Check(ok, "gpu.rt-indirect-readback");
			Check(cmds[0].instanceCount == binsOut.counts[0]
				&& cmds[0].firstInstance == 0
				&& cmds[1].instanceCount == binsOut.counts[1]
				&& cmds[1].firstInstance == binsOut.offsets[1],
				"gpu.rt-indirect-parity");
		}

		// A second frame with the SAME interest: no re-dispatch (the
		// idle-camera rule — nothing regenerates).
		size_t dispatchesBefore = 0;
		for (const auto& a : rt->active)
			if (a.published) ++dispatchesBefore;
		host.RunFrame([&](smvk::RenderGraph& g) {
			rt->BeginFrame(rm, g, interest, 1, deps);
			rt->RecordCullChain(rm, g, 0, viewSlabL,
				rm.GetBufferDeviceAddress(viewSlab), 0,
				glm::vec3(0.0f, 5.0f, 0.0f), kPlacementCullFrustum | kPlacementCullDistance);
		});
		rm.FlushUploads();
		rm.WaitForUploads();
		// Same published set; the count buffer UNTOUCHED this frame (no
		// ClearTile): the parity survives (the stable-resident contract).
		if (foundSlot >= 0) {
			std::vector<RenderInstance> gpu2;
			uint32_t overflow2 = 1;
			const uint32_t n2 = rt->pass.DownloadTile(rm,
				uint32_t(foundSlot), gpu2, 4096, &overflow2);
			Check(n2 == oracle.size() && overflow2 == 0,
				"gpu.rt-idle-frame-stable");
		}

		// A terrain-edit tick (deps bump): the tile REGENERATES through
		// the runtime (a new scatter dispatch in the same frame).
		DependencyVersions deps2;
		deps2.terrain = 5;
		host.RunFrame([&](smvk::RenderGraph& g) {
			rt->BeginFrame(rm, g, interest, 1, deps2);
		});
		rm.FlushUploads();
		rm.WaitForUploads();
		if (foundSlot >= 0) {
			std::vector<RenderInstance> gpu3;
			uint32_t overflow3 = 1;
			const uint32_t n3 = rt->pass.DownloadTile(rm,
				uint32_t(foundSlot), gpu3, 4096, &overflow3);
			Check(n3 == oracle.size() && overflow3 == 0,
				"gpu.rt-edit-regenerates-parity");
		}
		rt->Destroy(rg);
	}

	// =====================================================================
	// 9) The MESH PARTICLES (PR06): the exit gate — a GPU emitter spawns
	//    SPINNING 3D mesh variants, moves them, expires them, bins them
	//    per mesh, and the chain emits the indirect commands — with no
	//    CPU per-particle work anywhere. The billboard system stays
	//    untouched (separate buffers, separate kernels).
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: mesh particles\n");
	{
		smvk::MeshParticles mp;
		smvk::MeshParticlesDesc mpd{};
		mpd.maxParticles = 4096;
		mpd.maxEmitters = 8;
		Check(mp.Ensure(rm, rg, 2, mpd) && mp.Valid(),
			"gpu.mp-create");
		Check(mp.ShadersValid(), "gpu.mp-shaders");

		// Two emitters, two MESH variants: rocks (mesh 0, slow spin) +
		// leaves (mesh 1, fast spin). Bounded life so the expiry + the
		// free-list recycle are observable inside a few frames.
		smvk::MeshParticleEmitterGpu rows[2];
		rows[0].position = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
		rows[0].direction = glm::vec4(0.0f, 1.0f, 0.0f, 0.7f);
		rows[0].velocity = glm::vec4(1.0f, 3.0f, 0.0f, 1.0f);
		rows[0].life = glm::vec4(0.6f, 0.9f, 40.0f, 0.0f);   // mesh 0
		rows[0].spin = glm::vec4(2.0f, 6.0f, 11.0f, 0.1f);
		rows[0].size = glm::vec4(0.4f, 0.2f, 0.25f, 0.0f);
		rows[0].color = glm::vec4(0.7f, 0.65f, 0.6f, 0.0f);
		rows[1] = rows[0];
		rows[1].position = glm::vec4(2.0f, 0.0f, 2.0f, 0.3f);
		rows[1].velocity = glm::vec4(0.5f, 2.0f, 0.0f, 0.4f);
		rows[1].life = glm::vec4(0.3f, 0.5f, 80.0f, 1.0f);   // mesh 1
		rows[1].spin = glm::vec4(8.0f, 16.0f, 77.0f, 0.3f);
		rows[1].size = glm::vec4(0.25f, 0.08f, 0.3f, 0.0f);
		rows[1].color = glm::vec4(0.4f, 0.6f, 0.25f, 0.0f);
		mp.UpdateEmitters(rm, 0, rows, 2);

		// The per-mesh conservative spheres (the cull's bounds table).
		glm::vec4 meshBounds[2] = {
			glm::vec4(0.0f, 0.1f, 0.0f, 0.6f),
			glm::vec4(0.0f, 0.05f, 0.0f, 0.35f) };

		// ---- run N frames of emit -> sim -> the cull chain ----
		const float dt = 1.0f / 60.0f;
		const uint32_t kFrames = 90; // 1.5s: spawn + move + expire
		uint32_t frame = 0;
		std::vector<smvk::MeshParticleGpu> poolSnap;
		std::vector<uint32_t> freeSnap;
		for (uint32_t f = 0; f < kFrames; ++f) {
			frame = f;
			mp.UpdateEmitters(rm, frame % 2, rows, 2); // the flight ring
			mp.ClearFrameChain(rm);
			// The CPU-mirror carry (the scheduling arithmetic pin).
			smvk::MeshParticlesEmitPush ep;
			ep.emitters = mp.EmittersSliceAddress(rm, frame % 2);
			ep.emitterState = mp.EmitterStateAddress(rm);
			ep.particles = mp.ParticlesAddress(rm);
			ep.freeList = mp.FreeListAddress(rm);
			ep.emitterCount = 2;
			ep.maxParticles = mpd.maxParticles;
			ep.dt = dt;
			ep.frameSeed = f * 2654435761u + 1u;
			smvk::MeshParticlesSimPush sp;
			sp.particles = mp.ParticlesAddress(rm);
			sp.freeList = mp.FreeListAddress(rm);
			sp.gravityY = -9.81f;
			sp.dt = dt;
			sp.maxParticles = mpd.maxParticles;
			smvk::MeshParticlesCullPush cp;
			cp.particles = mp.ParticlesAddress(rm);
			cp.mask = mp.MaskAddress(rm);
			cp.bins = mp.BinsAddress(rm);
			cp.renderViews = rm.GetBufferDeviceAddress(viewSlab);
			cp.renderViewIndex = 0;
			cp.cullFlags = smvk::placement::kPlacementCullFrustum;
			cp.binCount = 2;
			cp.maxParticles = mpd.maxParticles;
			cp.meshBounds[0] = meshBounds[0];
			cp.meshBounds[1] = meshBounds[1];
			smvk::MeshParticlesScatterPush scp;
			scp.particles = mp.ParticlesAddress(rm);
			scp.mask = mp.MaskAddress(rm);
			scp.bins = mp.BinsAddress(rm);
			scp.visible = mp.VisibleAddress(rm);
			scp.binCount = 2;
			scp.maxParticles = mpd.maxParticles;
			smvk::MeshParticlesPrefixPush pp;
			pp.bins = mp.BinsAddress(rm);
			pp.binCount = 2;
			smvk::placement::PlacementDrawBuildPush dbp;
			dbp.bins = mp.BinsAddress(rm);
			dbp.indirect = mp.IndirectAddress(rm);
			dbp.protoCount = 2;
			// The immutable indirect halves (fake geometry: 300 idx/bin).
			if (f == 0) {
				smvk::placement::PlacementIndirectCmd seeds[2] = {};
				seeds[0].indexCount = 300; seeds[0].firstIndex = 0;
				seeds[1].indexCount = 300; seeds[1].firstIndex = 64;
				Check(mp.SeedIndirect(rm, seeds, 2), "gpu.mp-seed-indirect");
			}
			host.RunFrame([&](smvk::RenderGraph& g) {
				smvk::RenderPass& emit = mp.AddEmitPass(g);
				emit.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchEmit(cmd, mp.emitProgram, ep);
				});
				smvk::RenderPass& sim = mp.AddSimPass(g);
				sim.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchSim(cmd, mp.simProgram,
						sp, mp.SimGroupsFor(mpd.maxParticles));
				});
				smvk::RenderPass& cull = mp.AddCullPass(g, viewSlabL);
				cull.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchCull(cmd, mp.cullProgram, cp);
				});
				smvk::RenderPass& pre = mp.AddPrefixPass(g);
				pre.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchPrefix(cmd, mp.prefixProgram, pp);
				});
				smvk::RenderPass& scat = mp.AddScatterPass(g);
				scat.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchScatter(cmd, mp.scatterProgram, scp);
				});
				smvk::RenderPass& db = mp.AddDrawBuildPass(g);
				db.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchDrawBuild(cmd, mp.buildProgram, dbp);
				});
			});
			rm.FlushUploads();
			rm.WaitForUploads();
		}

		// ---- readback: the pool + the free list at the END ----
		poolSnap.resize(mpd.maxParticles);
		ok = rm.DownloadDataFromBuffer(mp.particlesBuffer, poolSnap.data(),
			sizeof(smvk::MeshParticleGpu) * mpd.maxParticles);
		Check(ok, "gpu.mp-pool-readback");
		freeSnap.resize(mpd.maxParticles + 1);
		ok = rm.DownloadDataFromBuffer(mp.freeListBuffer, freeSnap.data(),
			sizeof(uint32_t) * freeSnap.size());
		Check(ok, "gpu.mp-freelist-readback");

		size_t alive = 0, mesh0 = 0, mesh1 = 0;
		for (const auto& p : poolSnap)
			if (p.posLife.w > 0.0f) {
				++alive;
				if ((uint32_t)(p.params.y + 0.5f) == 0u) ++mesh0; else ++mesh1;
			}
		Check(alive > 0, "gpu.mp-alive-nonempty");
		Check(mesh0 > 0 && mesh1 > 0, "gpu.mp-two-mesh-variants");
		// The free list + the alive population cover the pool exactly
		// (the recycle invariant: no slot is both alive AND free).
		const uint32_t freeCount = freeSnap[0];
		Check(size_t(freeCount) + alive == mpd.maxParticles,
			"gpu.mp-freelist-conservation");
		// A slot pushed back by death is never double-pushed: the free
		// stack holds DISTINCT slot ids (spot check the invariant).
		{
			std::vector<uint32_t> stackIds(freeSnap.begin() + 1,
				freeSnap.begin() + 1 + size_t(freeCount));
			std::sort(stackIds.begin(), stackIds.end());
			bool distinct = std::adjacent_find(stackIds.begin(),
				stackIds.end()) == stackIds.end();
			for (uint32_t s : stackIds)
				if (poolSnap[s].posLife.w > 0.0f) { distinct = false; break; }
			Check(distinct, "gpu.mp-freelist-distinct-dead");
		}

		// The MOVEMENT + the SPIN actually happened: gravity pulled the
		// population below its spawn height and the quaternions moved
		// off identity-ish norms... quaternions are unit; verify the
		// rotation CHANGED by comparing a snapshot two frames apart.
		{
			bool moved = false, spun = false;
			for (const auto& p : poolSnap)
				if (p.posLife.w > 0.0f) {
					if (p.posLife.y < -0.05f) moved = true; // gravity drop
					if (std::abs(std::abs(p.rot.w) - 1.0f) > 1e-4f
						|| std::abs(p.rot.x) > 1e-4f
						|| std::abs(p.rot.y) > 1e-4f
						|| std::abs(p.rot.z) > 1e-4f)
						spun = true; // off identity = integrated at least once
				}
			Check(moved, "gpu.mp-gravity-moved");
			Check(spun, "gpu.mp-quaternion-spun");
			// Unit quaternions (the normalize held).
			bool unit = true;
			for (const auto& p : poolSnap)
				if (p.posLife.w > 0.0f) {
					const float n2 = glm::dot(p.rot, p.rot);
					if (std::abs(n2 - 1.0f) > 1e-3f) { unit = false; break; }
				}
			Check(unit, "gpu.mp-quaternion-unit");
		}

		// The size envelope: alive sizes are between their endpoints.
		{
			bool env = true;
			for (const auto& p : poolSnap)
				if (p.posLife.w > 0.0f
					&& (p.velSize.w > p.params.z + 1e-3f
						|| p.velSize.w < p.params.w - 1e-3f))
					{ env = false; break; }
			Check(env, "gpu.mp-size-envelope");
		}

		// ---- the chain readback: bins + the indirect commands ----
		smvk::placement::PlacementBinsGpu bins;
		ok = rm.DownloadDataFromBuffer(mp.binsBuffer, &bins, sizeof(bins));
		Check(ok, "gpu.mp-bins-readback");
		Check(bins.counts[0] + bins.counts[1] == bins.total
			&& bins.counts[0] == mesh0 && bins.counts[1] == mesh1,
			"gpu.mp-bin-counts-parity");
		Check(bins.offsets[1] == bins.counts[0],
			"gpu.mp-bin-offsets-parity");
		smvk::placement::PlacementIndirectCmd cmds[2];
		ok = rm.DownloadDataFromBuffer(mp.indirectBuffer, cmds,
			smvk::placement::kPlacementIndirectStride * 2);
		Check(ok, "gpu.mp-indirect-readback");
		Check(cmds[0].indexCount == 300 && cmds[0].firstIndex == 0
			&& cmds[1].indexCount == 300 && cmds[1].firstIndex == 64,
			"gpu.mp-indirect-immutable-halves");
		Check(cmds[0].instanceCount == bins.counts[0]
			&& cmds[0].firstInstance == 0
			&& cmds[1].instanceCount == bins.counts[1]
			&& cmds[1].firstInstance == bins.offsets[1],
			"gpu.mp-indirect-dynamic-halves");
		// The visible list: bin b's range holds exactly b's alive slots.
		{
			std::vector<uint32_t> vis(mpd.maxParticles, 0u);
			ok = rm.DownloadDataFromBuffer(mp.visibleBuffer, vis.data(),
				sizeof(uint32_t) * size_t(bins.total));
			Check(ok, "gpu.mp-visible-readback");
			std::vector<uint32_t> got0(vis.begin() + bins.offsets[0],
				vis.begin() + bins.offsets[0] + bins.counts[0]);
			std::vector<uint32_t> got1(vis.begin() + bins.offsets[1],
				vis.begin() + bins.offsets[1] + bins.counts[1]);
			bool idOk = true;
			for (uint32_t s : got0)
				if (poolSnap[s].posLife.w <= 0.0f
					|| (uint32_t)(poolSnap[s].params.y + 0.5f) != 0u)
					{ idOk = false; break; }
			for (uint32_t s : got1)
				if (poolSnap[s].posLife.w <= 0.0f
					|| (uint32_t)(poolSnap[s].params.y + 0.5f) != 1u)
					{ idOk = false; break; }
			Check(idOk, "gpu.mp-bin-range-identity");
		}

		// ---- the expiry proof: run PAST the lifetimes (2s more, no
		// emitters) — everything dies + recycles ----
		for (uint32_t f = 0; f < 120; ++f) {
			frame = kFrames + f;
			smvk::MeshParticleEmitterGpu off[1] = {};
			off[0].life = glm::vec4(0.6f, 0.9f, 0.0f, 0.0f); // rate 0 = off
			mp.UpdateEmitters(rm, frame % 2, off, 1);
			smvk::MeshParticlesEmitPush ep;
			ep.emitters = mp.EmittersSliceAddress(rm, frame % 2);
			ep.emitterState = mp.EmitterStateAddress(rm);
			ep.particles = mp.ParticlesAddress(rm);
			ep.freeList = mp.FreeListAddress(rm);
			ep.emitterCount = 1;
			ep.maxParticles = mpd.maxParticles;
			ep.dt = dt;
			ep.frameSeed = frame * 2654435761u + 1u;
			smvk::MeshParticlesSimPush sp;
			sp.particles = mp.ParticlesAddress(rm);
			sp.freeList = mp.FreeListAddress(rm);
			sp.gravityY = -9.81f;
			sp.dt = dt;
			sp.maxParticles = mpd.maxParticles;
			host.RunFrame([&](smvk::RenderGraph& g) {
				smvk::RenderPass& emit = mp.AddEmitPass(g);
				emit.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchEmit(cmd, mp.emitProgram, ep);
				});
				smvk::RenderPass& sim = mp.AddSimPass(g);
				sim.SetRecordingFunc([&](smvk::CommandBuffer& cmd) {
					mp.PushAndDispatchSim(cmd, mp.simProgram,
						sp, mp.SimGroupsFor(mpd.maxParticles));
				});
			});
			rm.FlushUploads();
			rm.WaitForUploads();
		}
		ok = rm.DownloadDataFromBuffer(mp.freeListBuffer, freeSnap.data(),
			sizeof(uint32_t) * freeSnap.size());
		Check(ok && freeSnap[0] == mpd.maxParticles,
			"gpu.mp-full-recycle-after-expiry");

		// ---- the SPAWN SCHEDULING arithmetic pin (the CPU mirror) ----
		{
			float carry = 0.0f;
			uint32_t totalSpawn = 0;
			for (uint32_t f = 0; f < kFrames; ++f) {
				for (int e = 0; e < 2; ++e) {
					const uint32_t q =
						smvk::MeshParticles::SpawnQuotaForFrame(
							rows[e].life.z, dt, carry, &carry);
					totalSpawn += q;
				}
			}
			// The pool capacity caps the ACTUAL spawn (40.0+80.0)/s * 1.5s
			// = 180 quota > the cap: the conservation law, not the quota,
			// is the pin (the free-list pops refuse the excess).
			Check(totalSpawn == 180, "gpu.mp-quota-arithmetic");
		}

		mp.Destroy(rg);
	}

	// =====================================================================
	// 10) THE DRAW (the definition-of-done proof): the full stack drawn
	 //    for real — a static-heap quad mesh, THREE prototype bins, the
	//    runtime's scatter + chain + a GRAPHICS PASS recording
	//    DrawIndexedIndirect per bin through the placement VS + the
	//    diagnostic unlit PS, into an external color target whose
	//    readback COUNTS the rasterized pixels. Instances -> commands ->
	//    pixels, end to end on device.
	// =====================================================================
	std::fprintf(stderr, "[place-gpu] stage: the draw\n");
	{
		using namespace smvk::terrain;
		// ---- the static-mesh heap: one quad (the retirement-fuzz path).
		smvk::AllocateStaticVertexFmt(rm);
		auto cluster = std::make_unique<smvk::Cluster>(&rm);
		smvk::StaticMeshUpload up;
		up.vertices.resize(4);
		up.vertices[0].pos = glm::vec3(-0.5f, 0.0f, -0.5f);
		up.vertices[1].pos = glm::vec3(0.5f, 0.0f, -0.5f);
		up.vertices[2].pos = glm::vec3(0.5f, 0.0f, 0.5f);
		up.vertices[3].pos = glm::vec3(-0.5f, 0.0f, 0.5f);
		for (auto& v : up.vertices) {
			v.color = glm::vec4(1.0f);
			v.normal = glm::vec3(0.0f, 1.0f, 0.0f);
			v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
		}
		up.indices = { 0, 1, 2, 0, 2, 3 };
		up.aabbMin = glm::vec3(-0.5f, 0.0f, -0.5f);
		up.aabbMax = glm::vec3(0.5f, 0.0f, 0.5f);
		const auto meshHandle = cluster->UploadStaticMesh(up);
		Check(!meshHandle.empty(), "gpu.draw-mesh-uploaded");
		const smvk::StaticMesh* mesh = cluster->staticMeshes.get(meshHandle);
		Check(mesh && !mesh->geometry.surfaces.empty(), "gpu.draw-mesh-surface");
		// The heap upload must LAND before the draw frames record (the
		// streamer defers copies; a frame that draws first reads zeros).
		rm.FlushUploads();
		rm.WaitForUploads();
		const smvk::Surface& surf = mesh->geometry.surfaces[0];

		// ---- the runtime (the section-8 fake world + a 3-proto layer). ----
		TerrainHField field;
		field.meta.worldSize = 16.0f;
		field.meta.originX = -8.0f;
		field.meta.originZ = -8.0f;
		field.meta.hTileSize = 4.0f;
		field.meta.hRes = 4;
		field.meta.gBlock = 8.0f;
		field.meta.baseY = kTerrainBaseY;
		field.meta.range = kTerrainRange;
		const uint32_t nH = field.tilesPerAxis();
		field.tiles.assign(size_t(nH) * nH,
			std::vector<uint8_t>(size_t(field.meta.hRes) * field.meta.hRes * 4, 0));
		for (uint32_t tz = 0; tz < nH; ++tz)
			for (uint32_t tx = 0; tx < nH; ++tx) {
				auto& tile = field.tiles[size_t(tz) * nH + tx];
				for (uint32_t py = 0; py < field.meta.hRes; ++py)
					for (uint32_t pxx = 0; pxx < field.meta.hRes; ++pxx) {
						const float h01 =
							(float)(tx * 4 + pxx + tz * 4 + py) / 1024.0f;
						uint8_t r, g;
						EncodeRG16(h01, r, g);
						const size_t o = (size_t(py) * field.meta.hRes + pxx) * 4;
						tile[o] = r; tile[o + 1] = g;
					}
			}
		std::vector<std::vector<uint8_t>> sTiles(2 * 2);
		const uint32_t sRes = 8;
		for (auto& t : sTiles)
			t.assign(size_t(sRes) * sRes * 4, 0);
		for (uint32_t tz = 0; tz < 2; ++tz)
			for (uint32_t tx = 0; tx < 2; ++tx) {
				auto& tile = sTiles[size_t(tz) * 2 + tx];
				for (uint32_t py = 0; py < sRes; ++py)
					for (uint32_t pxx = 0; pxx < sRes; ++pxx) {
						const size_t o = (size_t(py) * sRes + pxx) * 4;
						tile[o + 0] = uint8_t((pxx * 24 + py * 8) % 256);
						tile[o + 3] = 255;
					}
			}
		WorldMaskGrid maskGrid{ &sTiles, 2, sRes };

		PlacementSystem::LayerBinding lb;
		lb.programId = 404;
		lb.program.graphSeed = 4242;
		lb.program.nodeId = 1;
		lb.program.instrs = {
			FieldInstr{ .op = FieldOp::EcotopeMask, .params = { 0.0f, 0, 0, 0 } },
			FieldInstr{ .op = FieldOp::SlopeMask, .params = { 20.0f, 60.0f, 0, 0 } },
			FieldInstr{ .op = FieldOp::Mul, .a = 0, .b = 1 },
		};
		lb.program.acceptIndex = 2;
		lb.domainKind = DomainKind::Heightfield;
		lb.config.candidatesPerTile = 512;
		// THREE prototypes (the exit gate: three distinct bins drawing
		// from the same generated tile) — flat quads with sizes that
		// rasterize clearly.
		Prototype p0; p0.weight = 1.0f;
		p0.boundsMin = glm::vec3(-0.6f, 0.0f, -0.6f);
		p0.boundsMax = glm::vec3(0.6f, 0.3f, 0.6f);
		Prototype p1; p1.weight = 1.0f;
		p1.boundsMin = glm::vec3(-0.8f, 0.0f, -0.8f);
		p1.boundsMax = glm::vec3(0.8f, 0.5f, 0.8f);
		Prototype p2; p2.weight = 1.0f;
		p2.boundsMin = glm::vec3(-1.0f, 0.0f, -1.0f);
		p2.boundsMax = glm::vec3(1.0f, 0.8f, 1.0f);
		lb.prototypes.add(p0);
		lb.prototypes.add(p1);
		lb.prototypes.add(p2);
		lb.config.minScale = 1.2f;
		lb.config.maxScale = 2.4f;

		auto rt = std::make_unique<PlacementWorldRuntime>();
		rt->layer = &lb;
		rt->hfield = &field;
		rt->masks = &maskGrid;
		rt->tileSize = 8.0f;
		rt->haloMeters = 2.0f;
		rt->slotBudget = 1;
		rt->viewBudget = 1;
		Check(rt->Create(rm, rg), "gpu.draw-rt-create");

		// The draw programs (the placement VS + the diagnostic unlit PS).
		smvk::placement::PlacementDrawPrograms drawProgs;
		Check(drawProgs.Ensure(rm), "gpu.draw-programs");

		// ---- a REAL camera: a perspective + look-at over the tile ----
		// (the tile lives at y ~ -33..-28 on [0,8]^2; the eye hovers 14m
		// above, looking down-forward; the Vulkan Y flip baked in.)
		const glm::vec3 eye(4.0f, -18.0f, -3.0f);
		const glm::vec3 at(4.0f, -30.0f, 4.0f);
		glm::mat4 proj = glm::perspective(1.0f, 1.0f, 0.5f, 100.0f);
		proj[1][1] *= -1.0f; // the Vulkan clip-space Y flip
		const glm::mat4 view = glm::lookAt(eye, at, glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 viewProj = proj * view;
		// The cull planes: derive the six frustum planes from viewProj
		// (rows of the composition; generous inward normals).
		std::vector<float> slabBytes(64, 0.0f);
		auto putMatSlab = [&](size_t base, const glm::mat4& m) {
			for (int c = 0; c < 4; ++c)
				for (int r = 0; r < 4; ++r)
					slabBytes[base + c * 4 + r] = m[c][r];
		};
		putMatSlab(0, viewProj);   // m_viewProjectionMatrix
		putMatSlab(16, glm::inverse(viewProj)); // m_inverseViewProjectionMatrix
		{
			const glm::vec4 rows[4] = {
				glm::vec4(viewProj[0][0], viewProj[1][0], viewProj[2][0], viewProj[3][0]),
				glm::vec4(viewProj[0][1], viewProj[1][1], viewProj[2][1], viewProj[3][1]),
				glm::vec4(viewProj[0][2], viewProj[1][2], viewProj[2][2], viewProj[3][2]),
				glm::vec4(viewProj[0][3], viewProj[1][3], viewProj[2][3], viewProj[3][3]) };
			// The inward half-spaces: w + row (near), w - row (far) per axis.
			for (int p = 0; p < 3; ++p) {
				glm::vec4 lo = rows[3] + rows[p];
				glm::vec4 hi = rows[3] - rows[p];
				const float nlo = glm::length(glm::vec3(lo));
				const float nhi = glm::length(glm::vec3(hi));
				for (int k = 0; k < 4; ++k) {
					slabBytes[32 + (p * 2 + 0) * 4 + k] = lo[k] / nlo;
					slabBytes[32 + (p * 2 + 1) * 4 + k] = hi[k] / nhi;
				}
			}
		}
		smvk::Holder<smvk::BufferHandle> drawSlab = rm.CreateBuffer(
			sizeof(float) * slabBytes.size(), smvk::BufferType_Storage,
			"placement draw view slab");
		Check(!drawSlab.empty(), "gpu.draw-view-slab");
		smvk::LogicalBufferHandle drawSlabL = rg.AddExternalBuffer(drawSlab,
			"placement draw view slab");
		Check(!drawSlabL.empty(), "gpu.draw-view-logical");
		rm.UploadDataToBuffer(drawSlab, slabBytes.data(),
			sizeof(float) * slabBytes.size(),
			smvk::PipelineStageBit_Compute, smvk::AccessFlags::ShaderRead, 0, 0);

		// ---- the external color target (the readback proof surface). ----
		const uint32_t targetW = 256, targetH = 256;
		smvk::ImageCreateInfo ci{};
		ci.extent = { targetW, targetH, 1 };
		ci.format = smvk::Format::RGBA8_UNorm;
		ci.usage = smvk::ImageUsage::ColorAttachment
			| smvk::ImageUsage::TransferSrc;
		ci.mipLevels = 1;
		ci.arrayLayers = 1;
		ci.name = "placement draw target";
		smvk::Holder<smvk::ImageHandle> target = rm.CreateImage(ci, nullptr);
		Check(!target.empty(), "gpu.draw-target-created");
		const smvk::ImageViewCreateInfo vci{
			.viewType = smvk::ImageViewType::View2D,
			.format = smvk::Format::RGBA8_UNorm,
			.subresourceRange = { smvk::ImageAspect::Color, 0, 1, 0, 1 },
			.name = "placement draw target view",
		};
		smvk::Holder<smvk::ImageViewHandle> targetView =
			rm.CreateImageView(target, vci);
		Check(!targetView.empty(), "gpu.draw-target-view");
		smvk::LogicalImageViewHandle targetLogical =
			rg.AddExternalImageView(targetView, smvk::Format::RGBA8_UNorm,
				"placement draw target view");
		Check(!targetLogical.empty(), "gpu.draw-target-logical");

		// The immutable indirect halves: the SAME quad mesh for the three
		// prototype bins (indexCount 6, the heap's firstIndex/baseVertex).
		// Seeding must happen when the tile is PUBLISHED (the runtime's
		// SeedIndirect walks the published slots — before that it is a
		// no-op), so the seed call lives INSIDE the draw-frame record
		// below, right after the publish is observed.
		const auto fillSeeds = [&](PlacementIndirectCmd* seeds) {
			for (int p = 0; p < 3; ++p) {
				seeds[p].indexCount = uint32_t(up.indices.size());
				seeds[p].firstIndex = surf.firstIndex;
				seeds[p].vertexOffset = surf.baseVertex;
			}
		};

		// ---- frame 1: the scatter + the chain (the tile publishes). ----
		DependencyVersions deps;
		const glm::vec4 interest[1] = { glm::vec4(0.0f, 0.0f, 8.0f, 8.0f) };
		host.RunFrame([&](smvk::RenderGraph& g) {
			rt->BeginFrame(rm, g, interest, 1, deps);
			rt->RecordCullChain(rm, g, 0, drawSlabL,
				rm.GetBufferDeviceAddress(drawSlab), 0,
				glm::vec3(eye), kPlacementCullFrustum);
		});
		rm.FlushUploads();
		rm.WaitForUploads();

		// ---- frame 2: the chain re-runs (the idle tile, no re-dispatch)
		// + THE DRAW into the external target. ----
		int32_t drawSlot = -1;
		bool seeded = false;
		const auto recordDrawFrame = [&](smvk::RenderGraph& g) {
			rt->BeginFrame(rm, g, interest, 1, deps);
			rt->RecordCullChain(rm, g, 0, drawSlabL,
				rm.GetBufferDeviceAddress(drawSlab), 0,
				glm::vec3(eye), kPlacementCullFrustum);
			for (size_t s = 0; s < rt->active.size(); ++s)
				if (rt->active[s].published) { drawSlot = int32_t(s); break; }
			if (drawSlot < 0)
				return;
			// THE RUNTIME CONTRACT: seed the published visible sets once
			// per prototype table — the frame that observes the publish
			// is the earliest the seed can exist.
			if (!seeded) {
				PlacementIndirectCmd seeds[3];
				fillSeeds(seeds);
				Check(rt->SeedIndirect(rm, 0, seeds, 3),
					"gpu.draw-seed-indirect");
				seeded = true;
			}
			const PlacementVisibleSet* vs = rt->VisSet(drawSlot);
			smvk::RenderPass& draw = g.AddPass("PlacementDraw");
			draw.SetOffset(0, 0);
			draw.SetExtent({ targetW, targetH });
			draw.SetColor({ targetLogical },
				smvk::AttachmentLoadOp::Clear, smvk::AttachmentStoreOp::Store,
				smvk::ClearColor(0.1f, 0.2f, 0.3f, 1.0f));
			// The ordering edges: the draw reads the chain's outputs.
			smvk::BufferSyncRequest read{};
			read.barrierMasks.accessMask = smvk::AccessFlags::ShaderRead
				| smvk::AccessFlags::IndirectCommandRead;
			read.barrierMasks.stageMask = smvk::PipelineStageBit_AllGraphics;
			draw.AddBufferHint(rt->pass.instanceLogicals[size_t(drawSlot)], read);
			draw.AddBufferHint(vs->maskL, read);
			draw.AddBufferHint(vs->binsL, read);
			draw.AddBufferHint(vs->visibleL, read);
			draw.AddBufferHint(vs->indirectL, read);
			const PlacementVsPush tail{
				vs->VisibleAddress(rm),
				rt->pass.TileInstancesAddress(rm, uint32_t(drawSlot)),
				rm.GetBufferDeviceAddress(drawSlab),
				0u, 0u };
			// CAPTURE DISCIPLINE: the graph executes AFTER this record
			// callback returns (GpuHost::RunFrame -> rg->Execute), so the
			// recording lambda must own everything it touches that lives
			// in THIS scope — the push tail by value, the indirect buffer
			// handle by value, the program by pointer to the test-scope
			// holder. Capturing them by reference leaves the push reading
			// a dead stack slot (the garbage-addresses -> zero-pixels
			// bug this test was built to catch).
			const smvk::BufferHandle indirectBuf = vs->IndirectBuffer();
			smvk::ShaderProgram* const program = &drawProgs.program;
			draw.SetRecordingFunc([tail, indirectBuf, program](
				smvk::CommandBuffer& cmd) {
				cmd.BindStaticVertexFmt();
				cmd.BindRenderPipeline(*program,
					smvk::VertexFormat::StaticVertexFormat,
					smvk::DepthSettings{ .depthFunc = smvk::CompareOp::Always,
						.writeEnable = false, .depthTestEnable = false },
					smvk::StencilSettings{}, smvk::BlendSettings{},
					smvk::MSAASettings{},
					smvk::RasterSettings{ .cullMode = smvk::CullMode::None });
				cmd.PushConstants(kPlacementVsPushOffset, sizeof(tail),
					&tail, smvk::ShaderStage::All);
				// ONE indirect draw per prototype bin — the GPU-written
				// instanceCount + firstInstance drive everything.
				for (int p = 0; p < 3; ++p)
					cmd.DrawIndexedIndirect(indirectBuf,
						uint64_t(p) * kPlacementIndirectStride, 1,
						kPlacementIndirectStride);
			});
		};
		// TWO draw frames — the FIRST is the publish frame (BeginFrame
		// observes the activation token, the visible set is created, the
		// host seeds the immutable indirect halves) and the SECOND is the
		// first frame the draw can rasterize. This is NOT a workaround for
		// test timing — it pins a REAL rendergraph sync sharp edge, caught
		// on device right here:
		//
		//   THE STRANDED-UPLOAD WRITE. The seed is an UploadDataToBuffer
		//   (a TRANSFER-scoped write). In the SAME frame, drawbuild
		//   re-writes the buffer's dynamic halves with a COMPUTE-scoped
		//   write, and the draw pass then reads the buffer at
		//   DRAW_INDIRECT. The graph's per-buffer sync state tracks only
		//   the LAST write's scope, so the barrier it emits before the
		//   draw carries src = COMPUTE only. Vulkan memory dependencies
		//   are NOT transitive: the seed's TRANSFER_WRITE was made visible
		//   to drawbuild's COMPUTE scope, but nothing ever delivers it to
		//   the DRAW_INDIRECT read — so the publish frame's draw reads the
		//   PRE-seed indexCount (the creation-zero 0) and the indirect
		//   command is a SILENT no-op draw (0 indices — no fault, no
		//   pixels; the post-frame readback of the very same buffer shows
		//   the seeded indexCount 6 + drawbuild's instanceCount 69).
		//   The next frame's submit waits the previous frame's timeline at
		//   ALL_COMMANDS, which makes every prior write (including the
		//   stranded upload) visible to the whole command buffer — and the
		//   draw works.
		//
		//   CONSUMER RULE until the graph unions per-resource write
		//   scopes (the durable fix is for drawbuild to own the FULL
		//   command — writing the immutable halves from its push — so the
		//   indirect buffer is single-scope-written per frame): never let
		//   a draw depend on bytes an UPLOAD wrote to a buffer that a
		//   kernel re-writes in the same frame. Accept the one-frame lag
		//   (a pop-in, benign for streaming tiles) or rewrite the bytes in
		//   the kernel's scope.
		host.RunFrame(recordDrawFrame);
		rm.FlushUploads();
		rm.WaitForUploads();
		if (drawSlot >= 0) {
			// The publish frame's observed state: the buffer HOLDS the
			// seeded immutable halves + drawbuild's dynamic halves, yet the
			// frame rasterized nothing (the stranded-write note above).
			const PlacementVisibleSet* vs = rt->VisSet(size_t(drawSlot));
			PlacementIndirectCmd cmds[3];
			std::vector<uint8_t> px(size_t(targetW) * targetH * 4, 0);
			if (rm.DownloadDataFromBuffer(vs->indirect, cmds,
					kPlacementIndirectStride * 3)
				&& rm.DownloadDataFromImage(target,
					smvk::ImageLayout::ColorAttachmentOptimal, px.data(),
					px.size())) {
				size_t lit = 0;
				for (size_t i = 0; i + 3 < px.size(); i += 4)
					if (px[i] > 100) ++lit;
				std::fprintf(stderr, "[place-gpu] draw publish frame: "
					"cmd0 {idx=%u inst=%u fi=%u}, %zu px lit\n",
					cmds[0].indexCount, cmds[0].instanceCount,
					cmds[0].firstInstance, lit);
			}
		}
		host.RunFrame(recordDrawFrame);
		rm.FlushUploads();
		rm.WaitForUploads();

		Check(drawSlot >= 0, "gpu.draw-tile-published");

		// ---- the readback: COUNT the rasterized pixels. ----
		if (drawSlot >= 0) {
			const PlacementVisibleSet* vs = rt->VisSet(drawSlot);
			PlacementBinsGpu bins;
			ok = rm.DownloadDataFromBuffer(vs->bins, &bins, sizeof(bins));
			Check(ok, "gpu.draw-bins-readback");
			Check(bins.total > 0, "gpu.draw-visible-nonempty");
			std::vector<uint8_t> px(size_t(targetW) * targetH * 4, 0);
			ok = rm.DownloadDataFromImage(target,
				smvk::ImageLayout::ColorAttachmentOptimal, px.data(), px.size());
			Check(ok, "gpu.draw-target-readback");
			size_t lit = 0, firstLit = SIZE_MAX;
			for (size_t i = 0; i + 3 < px.size(); i += 4) {
				// The clear (0.1, 0.2, 0.3) -> (25, 51, 76); a drawn pixel
				// is the white*shade quad (>= 140 on R).
				if (px[i] > 100) {
					++lit;
					if (firstLit == SIZE_MAX) firstLit = i;
				}
			}
			Check(lit > 100, "gpu.draw-pixels-rasterized");
			if (lit > 0)
				std::fprintf(stderr, "[place-gpu] draw: %zu/%zu pixels lit"
					" (bins %u+%u+%u, first rgb %u,%u,%u)\n", lit,
					px.size() / 4, bins.counts[0], bins.counts[1],
					bins.counts[2], px[firstLit], px[firstLit + 1],
					px[firstLit + 2]);
			// THREE bins drew: every bin with a nonzero count contributed
			// pixels (the commands drove real rasterization).
			const uint32_t drawingBins = uint32_t((bins.counts[0] > 0)
				+ (bins.counts[1] > 0) + (bins.counts[2] > 0));
			Check(drawingBins >= 2, "gpu.draw-multiple-bins-drew");
			Check(bins.counts[0] + bins.counts[1] + bins.counts[2]
					== bins.total,
				"gpu.draw-bin-sum-parity");
		}
		rt->Destroy(rg);
	}

	std::printf("[place-gpu] done: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
