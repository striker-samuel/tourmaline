// placement_tests: Phase-0 foundation for PlacementGraph.
// Hand-rolled runner (repo convention): PASS/FAIL lines, nonzero exit on failure.
// Covers: stable-ID determinism, channel independence, prototype selection,
// SoA point storage, RenderInstance ABI vs HLSL mirror.

#include <cstdint>
#include <cmath>
#include <iostream>
#include <vector>

#include <Engine/Placement/placement_ids.h>
#include <Engine/Placement/placement_point.h>
#include <Engine/Placement/placement_prototype.h>
#include <Engine/Placement/placement_cache.h>
#include <Engine/Placement/placement_domain.h>
#include <Engine/Placement/placement_eval.h>
#include <Engine/Placement/placement_gpu.h>
#include <Engine/Placement/placement_ir.h>
#include <Engine/Placement/placement_override.h>
#include <Engine/Placement/placement_physics.h>
#include <Engine/Placement/placement_tile.h>

namespace {
int g_failures = 0;
int g_checks = 0;
void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) std::cout << "PASS " << name << "\n";
	else { ++g_failures; std::cout << "FAIL " << name << "\n"; }
}
using smvk::placement::RandomChannel;
using smvk::placement::StableID;
using smvk::placement::TileID;
}

void TestStableDeterminism() {
	TileID t{ 3, -1, 7 };
	StableID a = smvk::placement::MakeStableID(1234, 5, t, 9, 42);
	StableID b = smvk::placement::MakeStableID(1234, 5, t, 9, 42);
	Check(a == b, "place.id.deterministic");
	StableID c = smvk::placement::MakeStableID(1234, 5, t, 9, 43);
	Check(a != c, "place.id.candidate-varies");
	StableID d = smvk::placement::MakeStableID(1235, 5, t, 9, 42);
	Check(a != d, "place.id.seed-varies");
	TileID t2{ 4, -1, 7 };
	StableID e = smvk::placement::MakeStableID(1234, 5, t2, 9, 42);
	Check(a != e, "place.id.tile-varies");
	// lanes uncorrelated for neighbours
	Check(a.w[0] != a.w[1] || a.w[2] != a.w[3], "place.id.lanes-split");
}

void TestChannels() {
	TileID t{ 0, 0, 0 };
	StableID id = smvk::placement::MakeStableID(77, 1, t, 0, 0);
	float rPos = smvk::placement::Random01(id, RandomChannel::Position);
	float rVar = smvk::placement::Random01(id, RandomChannel::Variant);
	float rDen = smvk::placement::Random01(id, RandomChannel::Density);
	Check(rPos >= 0.0f && rPos < 1.0f, "place.rng.range-pos");
	Check(rVar >= 0.0f && rVar < 1.0f, "place.rng.range-var");
	Check(!(rPos == rVar && rVar == rDen), "place.rng.channels-independent");
	// Repeatability
	float rPos2 = smvk::placement::Random01(id, RandomChannel::Position);
	Check(rPos == rPos2, "place.rng.repeatable");
	// 24-bit quantization: value * 2^24 is integral
	float scaled = rPos * 16777216.0f;
	Check(scaled == (float)(uint32_t)scaled, "place.rng.24bit-exact");
	// Priority is integer, deterministic
	Check(smvk::placement::PriorityOf(id) == smvk::placement::PriorityOf(id),
		"place.rng.priority-deterministic");
}

void TestPrototypeSelect() {
	smvk::placement::PrototypeTable table;
	Check(!table.select(0.5f).has_value(), "place.proto.empty-nullopt");
	smvk::placement::Prototype oak; oak.assetUri = "oak"; oak.weight = 50.0f;
	smvk::placement::Prototype pine; pine.assetUri = "pine"; pine.weight = 30.0f;
	smvk::placement::Prototype birch; birch.assetUri = "birch"; birch.weight = 20.0f;
	table.add(oak); table.add(pine); table.add(birch);
	Check(table.totalWeight() == 100.0f, "place.proto.total");
	Check(table.select(0.0f).value() == 0, "place.proto.first-bucket");
	Check(table.select(0.49f).value() == 0, "place.proto.oak-range");
	Check(table.select(0.5f).value() == 1, "place.proto.pine-range");
	Check(table.select(0.799f).value() == 1, "place.proto.pine-edge");
	Check(table.select(0.8f).value() == 2, "place.proto.birch-range");
	Check(table.select(0.999f).value() == 2, "place.proto.last-bucket");
	// Zero-weight entry never selected
	smvk::placement::PrototypeTable t2;
	smvk::placement::Prototype z; z.assetUri = "z"; z.weight = 0.0f;
	smvk::placement::Prototype nz; nz.assetUri = "nz"; nz.weight = 1.0f;
	t2.add(z); t2.add(nz);
	bool onlySecond = true;
	for (int i = 0; i < 16; ++i)
		onlySecond = onlySecond && (t2.select(i / 16.0f).value() == 1);
	Check(onlySecond, "place.proto.zero-weight-skipped");
}

void TestPointsSoA() {
	smvk::placement::PlacementPoints pts;
	pts.reserve(4);
	TileID t{ 1, 2, 3 };
	for (uint32_t i = 0; i < 3; ++i) {
		StableID id = smvk::placement::MakeStableID(9, 2, t, 0, i);
		pts.push(glm::vec3((float)i, 0, 0), glm::quat(1, 0, 0, 0),
			glm::vec3(1), i % 2, id);
	}
	Check(pts.size() == 3, "place.soa.size");
	Check(pts.position.size() == pts.stableId.size(), "place.soa.parallel");
	Check(pts.stableId[0] != pts.stableId[1], "place.soa.stable-unique");
	pts.clear();
	Check(pts.size() == 0, "place.soa.clear");
}

void TestRenderInstanceAbi() {
	Check(sizeof(smvk::placement::RenderInstance) == 40, "place.abi.instance-40B");
	Check(offsetof(smvk::placement::RenderInstance, yaw) == 12, "place.abi.yaw-offset");
	Check(offsetof(smvk::placement::RenderInstance, scale) == 16, "place.abi.scale-offset");
	Check(offsetof(smvk::placement::RenderInstance, prototypeId) == 28, "place.abi.proto-offset");
	Check(offsetof(smvk::placement::RenderInstance, stableLow) == 32, "place.abi.stable-offset");
	Check(offsetof(smvk::placement::RenderInstance, customB) == 36, "place.abi.custom-offset");
	// PlacementRef is params-only: no per-instance data
	smvk::placement::PlacementRef ref;
	Check(ref.densityScale == 1.0f && ref.tileSize == 64.0f, "place.ref.defaults");
}

void TestGridDomain() {
	smvk::placement::GridDomain grid;
	grid.origin = glm::vec3(0, 5, 0);
	grid.extent = glm::vec2(64.0f, 64.0f);
	grid.cellsX = 8; grid.cellsZ = 8;
	smvk::placement::TileID t{ 0, 0, 0 };
	// Deterministic: same candidate -> same sample
	smvk::placement::StableID id = smvk::placement::MakeStableID(1, 0, t, 0, 0);
	auto a = grid.sample(id, 0);
	auto b = grid.sample(id, 0);
	Check(a.p == b.p, "place.grid.deterministic");
	// Inside the tile rect, on the base plane
	Check(a.p.x >= 0.0f && a.p.x < 64.0f && a.p.z >= 0.0f && a.p.z < 64.0f,
		"place.grid.in-bounds");
	Check(a.p.y == 5.0f, "place.grid.base-height");
	Check(a.n == glm::vec3(0, 1, 0), "place.grid.up-normal");
	// Different cells land in different 8m bins
	auto c = grid.sample(smvk::placement::MakeStableID(1, 0, t, 0, 9), 9);
	Check((int)(a.p.x / 8.0f) != (int)(c.p.x / 8.0f) || (int)(a.p.z / 8.0f) != (int)(c.p.z / 8.0f),
		"place.grid.cell-spread");
}

void TestMeshAlias() {
	// Two triangles: big (area 50) + small (area 0.5). Alias table must
	// route ~99% of picks to the big one.
	smvk::placement::PlacementMeshData mesh;
	mesh.positions = { glm::vec3(0,0,0), glm::vec3(10,0,0), glm::vec3(0,0,10),
		glm::vec3(20,0,0), glm::vec3(21,0,0), glm::vec3(20,0,1) };
	mesh.indices = { 0, 1, 2, 3, 4, 5 };
	smvk::placement::BuildMeshAliasTable(mesh);
	Check(mesh.hasAlias(), "place.mesh.alias-built");
	Check(mesh.triangleCount() == 2, "place.mesh.tri-count");
	int bigHits = 0;
	const int N = 2000;
	for (int i = 0; i < N; ++i) {
		smvk::placement::StableID id =
			smvk::placement::MakeStableID(3, 0, smvk::placement::TileID{0,0,0}, 0, (uint32_t)i);
		auto s = smvk::placement::SampleMeshSurface(mesh, id);
		if (s.primitiveId == 0) ++bigHits;
		// Point must lie on the triangle plane (y==0) and inside bounds
		if (!(s.p.y == 0.0f)) { Check(false, "place.mesh.on-plane"); break; }
	}
	Check(bigHits > N * 95 / 100, "place.mesh.area-weighted");
	// Degenerate mesh: uniform fallback, no crash
	smvk::placement::PlacementMeshData deg;
	deg.positions = { glm::vec3(0), glm::vec3(0), glm::vec3(0) };
	deg.indices = { 0, 0, 0 };
	smvk::placement::BuildMeshAliasTable(deg);
	auto ds = smvk::placement::SampleMeshSurface(deg,
		smvk::placement::MakeStableID(1, 0, smvk::placement::TileID{}, 0, 0));
	Check(ds.primitiveId == 0, "place.mesh.degenerate-fallback");
}

void TestHeightfield() {
	smvk::placement::HeightfieldData hf;
	hf.width = 4; hf.height = 4;
	hf.origin = glm::vec3(0, 0, 0);
	hf.spacing = glm::vec2(1.0f, 1.0f);
	// Plane h = x + 2z
	hf.heights.resize(16);
	for (uint32_t z = 0; z < 4; ++z)
		for (uint32_t x = 0; x < 4; ++x)
			hf.heights[z * 4 + x] = (float)x + 2.0f * (float)z;
	Check(hf.valid(), "place.hf.valid");
	Check(hf.sampleHeight(1.0f, 1.0f) == 3.0f, "place.hf.exact-texel");
	Check(std::abs(hf.sampleHeight(0.5f, 0.0f) - 0.5f) < 1e-5f, "place.hf.bilinear");
	glm::vec3 n = hf.sampleNormal(1.5f, 1.5f);
	// Plane normal: normalize(-1, 1, -2)
	glm::vec3 expect = glm::normalize(glm::vec3(-1, 1, -2));
	Check(glm::length(n - expect) < 1e-4f, "place.hf.normal");
	smvk::placement::HeightfieldDomain dom;
	dom.field = &hf;
	dom.extentMin = glm::vec2(0, 0); dom.extentMax = glm::vec2(3, 3);
	auto s = dom.sample(smvk::placement::MakeStableID(5, 0, smvk::placement::TileID{}, 0, 0));
	Check(s.p.x >= 0 && s.p.x <= 3 && s.p.z >= 0 && s.p.z <= 3, "place.hf.domain-bounds");
	Check(std::abs(s.p.y - hf.sampleHeight(s.p.x, s.p.z)) < 1e-5f, "place.hf.domain-height");
}

void TestFieldEval();
void TestAcceptance();
void TestNoise();
void TestTileDeterminism();
void TestTileStreamingOrder();
void TestMinDistance();
void TestTileCache();
void TestTransientDraws();
void TestGpuPushAbi();
void TestOverrides();
void TestExclusions();
void TestCacheRoundTrip();
void TestPhysicsQuery();
void TestPromotion();

void TestFieldEval() {
	using namespace smvk::placement;
	// density = 0.5 * slopeMask(flat=0deg, steep=40deg)
	PlacementProgram prog;
	prog.graphSeed = 11;
	FieldInstr c; c.op = FieldOp::Const; c.params[0] = 0.5f;
	FieldInstr sm; sm.op = FieldOp::SlopeMask; sm.params[0] = 0.0f; sm.params[1] = 40.0f;
	FieldInstr mul; mul.op = FieldOp::Mul; mul.a = 0; mul.b = 1;
	prog.instrs = { c, sm, mul };
	prog.acceptIndex = 2;
	Check(prog.valid(), "place.ir.valid");
	DomainSample flat; flat.p = glm::vec3(0, 10, 0); flat.n = glm::vec3(0, 1, 0);
	auto v = EvaluateProgram(prog, flat);
	Check(std::abs(v[2] - 0.5f) < 1e-5f, "place.eval.flat-density");
	DomainSample wall; wall.p = glm::vec3(0, 10, 0); wall.n = glm::vec3(1, 0, 0);
	auto v2 = EvaluateProgram(prog, wall);
	Check(v2[2] < 1e-5f, "place.eval.steep-rejected");
	// Remap + smoothstep sanity: remap(0.5 from [0,1] to [10,20]) = 15
	PlacementProgram p2;
	FieldInstr c2 = OpConst(0.5f);
	FieldInstr r; r.op = FieldOp::Remap; r.a = 0;
	r.params[0] = 0; r.params[1] = 1; r.params[2] = 10; r.params[3] = 20;
	p2.instrs = { c2, r }; p2.acceptIndex = 1;
	auto v3 = EvaluateProgram(p2, flat);
	Check(std::abs(v3[1] - 15.0f) < 1e-4f, "place.eval.remap");
	// AltitudeMask: y=10 inside [0,5]x[15,25] band -> ~1; y=100 -> 0
	PlacementProgram p3;
	FieldInstr am; am.op = FieldOp::AltitudeMask;
	am.params[0] = 0; am.params[1] = 5; am.params[2] = 15; am.params[3] = 25;
	p3.instrs = { am }; p3.acceptIndex = 0;
	DomainSample mid; mid.p = glm::vec3(0, 10, 0); mid.n = glm::vec3(0, 1, 0);
	DomainSample high; high.p = glm::vec3(0, 100, 0); high.n = glm::vec3(0, 1, 0);
	Check(EvaluateProgram(p3, mid)[0] > 0.99f, "place.eval.alt-inside");
	Check(EvaluateProgram(p3, high)[0] < 1e-5f, "place.eval.alt-outside");
	// Transform helpers use own channels: yaw in [-pi,pi], scale in range
	StableID id = MakeStableID(1, 0, TileID{}, 0, 0);
	float yaw = YawFromId(id);
	Check(yaw >= -3.14159265f && yaw <= 3.14159265f, "place.eval.yaw-range");
	float s = UniformScaleFromId(id, 0.8f, 1.2f);
	Check(s >= 0.8f && s < 1.2001f, "place.eval.scale-range");
}

void TestAcceptance() {
	using namespace smvk::placement;
	TileID t{ 0, 0, 0 };
	// Stability: accepted(0.3) ⊆ accepted(0.4) — raising density only reveals.
	int lowCount = 0, highCount = 0, violations = 0;
	for (uint32_t i = 0; i < 2000; ++i) {
		StableID id = MakeStableID(9, 1, t, 0, i);
		bool low = AcceptCandidate(0.3f, id);
		bool high = AcceptCandidate(0.4f, id);
		lowCount += low; highCount += high;
		if (low && !high) ++violations;
	}
	Check(violations == 0, "place.accept.stable-reveal");
	Check(lowCount > 400 && lowCount < 800, "place.accept.low-rate");
	Check(highCount > lowCount, "place.accept.high-grows");
	// Edges
	StableID id = MakeStableID(1, 0, t, 0, 0);
	Check(!AcceptCandidate(0.0f, id), "place.accept.zero");
	Check(AcceptCandidate(1.0f, id), "place.accept.one");
	// Quantization: tiny float wobble cannot flip the decision
	float d = 0.374f;
	bool a1 = AcceptCandidate(d, id);
	bool a2 = AcceptCandidate(d + 1e-7f, id);
	Check(a1 == a2, "place.accept.quant-stable");
}

void TestNoise() {
	using namespace smvk::placement;
	float n0 = ValueNoise2D(3.25f, 7.75f, 42);
	Check(n0 >= 0.0f && n0 <= 1.0f, "place.noise.range");
	Check(ValueNoise2D(3.25f, 7.75f, 42) == n0, "place.noise.deterministic");
	Check(ValueNoise2D(3.25f, 7.75f, 43) != n0, "place.noise.seed-varies");
	// Lattice points interpolate exactly to the hashed corner
	float corner = ValueNoise2D(2.0f, 5.0f, 42);
	Check(corner >= 0.0f && corner <= 1.0f, "place.noise.corner-range");
	// Negative coords don't crash / stay in range (HLSL parity path)
	float neg = ValueNoise2D(-12.5f, -3.25f, 42);
	Check(neg >= 0.0f && neg <= 1.0f, "place.noise.negative-range");
}

namespace {
// Builds a constant-density grid layer for tile tests. Caller owns grid.
smvk::placement::PlacementSystem::LayerBinding MakeGridLayer(
	smvk::placement::GridDomain& grid, float density, uint32_t candidates) {
	using namespace smvk::placement;
	PlacementSystem::LayerBinding lb;
	lb.programId = 101;
	lb.program.graphSeed = 2024;
	lb.program.nodeId = 7;
	lb.program.instrs = { OpConst(density) };
	lb.program.acceptIndex = 0;
	lb.domainKind = DomainKind::Grid;
	lb.grid = &grid;
	lb.config.candidatesPerTile = candidates;
	return lb;
}
}

void TestTileDeterminism() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.origin = glm::vec3(0, 0, 0);
	grid.extent = glm::vec2(64, 64);
	grid.cellsX = 16; grid.cellsZ = 16;
	PlacementSystem sys;
	sys.registerLayer(MakeGridLayer(grid, 0.5f, 1024));
	TileID t{ 2, 0, 3 };
	DependencyVersions deps;
	Check(sys.generateTile(101, t, deps) > 0, "place.tile.nonempty");
	const PlacementTile* a = sys.findTile(101, t);
	size_t n0 = a ? a->instances.size() : 0;
	// Regenerate in place: bit-identical positions + prototypes.
	sys.generateTile(101, t, deps);
	const PlacementTile* b = sys.findTile(101, t);
	bool identical = b && b->instances.size() == n0;
	if (identical) {
		for (size_t i = 0; i < n0; ++i) {
			if (a->instances[i].position != b->instances[i].position ||
				a->instances[i].prototypeId != b->instances[i].prototypeId) {
				identical = false; break;
			}
		}
	}
	Check(identical, "place.tile.regen-identical");
	// Fresh system, same inputs: identical too (no hidden global RNG).
	PlacementSystem sys2;
	GridDomain grid2 = grid;
	sys2.registerLayer(MakeGridLayer(grid2, 0.5f, 1024));
	sys2.generateTile(101, t, deps);
	const PlacementTile* c = sys2.findTile(101, t);
	Check(c && c->instances.size() == n0, "place.tile.fresh-identical-count");
}

void TestTileStreamingOrder() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.cellsX = 8; grid.cellsZ = 8;
	PlacementSystem a, b;
	GridDomain g2 = grid;
	a.registerLayer(MakeGridLayer(grid, 0.6f, 512));
	b.registerLayer(MakeGridLayer(g2, 0.6f, 512));
	DependencyVersions deps;
	TileID t1{ 0, 0, 0 }, t2{ 1, 0, 0 }, t3{ 0, 0, 1 };
	a.generateTile(101, t1, deps);
	a.generateTile(101, t2, deps);
	a.generateTile(101, t3, deps);
	b.generateTile(101, t3, deps);
	b.generateTile(101, t1, deps);
	b.generateTile(101, t2, deps);
	// Tile t1 identical regardless of generation order (stable hashing).
	const PlacementTile* ta = a.findTile(101, t1);
	const PlacementTile* tb = b.findTile(101, t1);
	bool same = ta && tb && ta->instances.size() == tb->instances.size();
	if (same) {
		for (size_t i = 0; i < ta->instances.size(); ++i)
			if (ta->instances[i].position != tb->instances[i].position) { same = false; break; }
	}
	Check(same, "place.tile.order-independent");
}

void TestMinDistance() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.cellsX = 32; grid.cellsZ = 32;
	PlacementSystem sys;
	auto lb = MakeGridLayer(grid, 1.0f, 2048); // accept-all: maximum conflict
	lb.config.minSpacing = 4.0f;
	sys.registerLayer(lb);
	DependencyVersions deps;
	size_t n = sys.generateTile(101, TileID{ 0, 0, 0 }, deps);
	Check(n > 0 && n < 2048, "place.mindist.thins");
	const PlacementTile* t = sys.findTile(101, TileID{ 0, 0, 0 });
	bool ok = true;
	if (t) {
		for (size_t i = 0; i < t->instances.size() && ok; ++i)
			for (size_t j = i + 1; j < t->instances.size(); ++j) {
				glm::vec3 d = t->instances[i].position - t->instances[j].position;
				if (glm::dot(d, d) < 4.0f * 4.0f * 0.999f) { ok = false; break; }
			}
	}
	Check(ok, "place.mindist.spacing-holds");
	// Deterministic across regens.
	sys.generateTile(101, TileID{ 0, 0, 0 }, deps);
	Check(sys.findTile(101, TileID{ 0, 0, 0 })->instances.size() == n,
		"place.mindist.regen-stable");
}

void TestTileCache() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.cellsX = 4; grid.cellsZ = 4;
	PlacementSystem sys;
	sys.registerLayer(MakeGridLayer(grid, 0.9f, 128));
	DependencyVersions deps;
	sys.generateTile(101, TileID{ 0, 0, 0 }, deps);
	sys.generateTile(101, TileID{ 1, 0, 0 }, deps);
	sys.generateTile(101, TileID{ 2, 0, 0 }, deps);
	Check(sys.residentTileCount() == 3, "place.cache.three-resident");
	Check(sys.residentInstanceCount(101) > 0, "place.cache.instances-counted");
	sys.invalidateLayer(101);
	Check(sys.findTile(101, TileID{ 0, 0, 0 })->state == TileState::Dirty,
		"place.cache.invalidate-dirty");
	sys.generateTile(101, TileID{ 0, 0, 0 }, deps); // refresh clears dirty
	Check(sys.findTile(101, TileID{ 0, 0, 0 })->state == TileState::Resident,
		"place.cache.refresh-resident");
	sys.evictBeyond(1);
	Check(sys.residentTileCount() == 1, "place.cache.evict-budget");
}

void TestTransientDraws() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.origin = glm::vec3(0, 0, 0);
	grid.extent = glm::vec2(64, 64);
	grid.cellsX = 8; grid.cellsZ = 8;
	PlacementSystem sys;
	sys.registerLayer(MakeGridLayer(grid, 1.0f, 256));
	DependencyVersions deps;
	sys.generateTile(101, TileID{ 0, 0, 0 }, deps);
	size_t total = sys.residentInstanceCount(101);
	std::vector<TransientDraw> all;
	sys.pushVisibleDraws(101, nullptr, all); // no cull: everything
	Check(all.size() == total, "place.draws.no-cull-all");
	std::vector<TransientDraw> none;
	sys.pushVisibleDraws(101, [](const glm::vec3&, float) { return false; }, none);
	Check(none.empty(), "place.draws.full-cull-empty");
	// Half-space cull keeps a strict subset with matching prototype IDs.
	std::vector<TransientDraw> half;
	sys.pushVisibleDraws(101,
		[](const glm::vec3& c, float) { return c.x < 32.0f; }, half);
	Check(!half.empty() && half.size() < all.size(), "place.draws.partial-cull");
	bool protoOk = true;
	for (auto& d : half) if (d.prototypeId != 0) { protoOk = false; break; }
	Check(protoOk, "place.draws.proto-passthrough");
}

void TestGpuPushAbi() {
	using namespace smvk::placement;
	Check(sizeof(PlacementTilePush) == 144, "place.gpu.push-144B");
	Check(offsetof(PlacementTilePush, candidateCount) == 28, "place.gpu.count-offset");
	Check(offsetof(PlacementTilePush, gridOrigin) == 48, "place.gpu.grid-offset");
	Check(offsetof(PlacementTilePush, density) == 80, "place.gpu.density-offset");
	Check(offsetof(PlacementTilePush, weightsA) == 112, "place.gpu.weights-offset");
	Check(PlacementDispatchGroups(64) == 1, "place.gpu.dispatch-exact");
	Check(PlacementDispatchGroups(65) == 2, "place.gpu.dispatch-roundup");
	Check(PlacementDispatchGroups(4096) == 64, "place.gpu.dispatch-4k");
}

void TestOverrides() {
	using namespace smvk::placement;
	// Three instances with known IDs; move one, delete one, swap one's proto.
	std::vector<RenderInstance> inst(3);
	std::vector<StableID> ids;
	for (int i = 0; i < 3; ++i) {
		inst[i].position = glm::vec3((float)i, 0, 0);
		inst[i].prototypeId = 0;
		ids.push_back(MakeStableID(1, 0, TileID{}, 0, (uint32_t)i));
	}
	OverrideLayer layer;
	PlacementOverride mv; mv.target = ids[0]; mv.position = glm::vec3(99, 0, 0);
	layer.set(mv);
	PlacementOverride del; del.target = ids[1]; del.deleted = true;
	layer.set(del);
	PlacementOverride sw; sw.target = ids[2]; sw.prototype = 5;
	layer.set(sw);
	Check(layer.size() == 3, "place.override.count");
	layer.apply(inst, ids);
	Check(inst.size() == 2 && ids.size() == 2, "place.override.delete-compacts");
	Check(inst[0].position.x == 99.0f, "place.override.move");
	Check(ids[0] == MakeStableID(1, 0, TileID{}, 0, 0), "place.override.keeps-stable-id");
	Check(inst[1].prototypeId == 5, "place.override.proto-swap");
	// Regen safety: override addresses the same StableID after regen because
	// IDs derive from seed/node/tile/prim/index, not array position.
	Check(ids[1] == MakeStableID(1, 0, TileID{}, 0, 2), "place.override.regen-safe");
	// Empty layer is a no-op.
	OverrideLayer empty;
	size_t n = inst.size();
	empty.apply(inst, ids);
	Check(inst.size() == n, "place.override.empty-noop");
}

void TestExclusions() {
	using namespace smvk::placement;
	std::vector<RenderInstance> inst(4);
	std::vector<StableID> ids;
	for (int i = 0; i < 4; ++i) {
		inst[i].position = glm::vec3((float)(i * 10), 0, 0);
		ids.push_back(MakeStableID(2, 0, TileID{}, 0, (uint32_t)i));
	}
	// Disc at x=10 r=6 kills instance 1 only (x=0,20,30 survive).
	size_t kept = ApplyExclusions(inst, ids, { ExclusionDisc{ glm::vec2(10, 0), 6.0f } });
	Check(kept == 3, "place.excl.disc-kills-one");
	Check(inst[1].position.x == 20.0f, "place.excl.compact-order");
	Check(ids[1] == MakeStableID(2, 0, TileID{}, 0, 2), "place.excl.ids-track");
	Check(ApplyExclusions(inst, ids, {}) == 3, "place.excl.empty-noop");
}

void TestCacheRoundTrip() {
	using namespace smvk::placement;
	std::vector<RenderInstance> inst(2);
	std::vector<StableID> ids;
	inst[0].position = glm::vec3(1, 2, 3); inst[0].prototypeId = 4; inst[0].stableLow = 77;
	ids.push_back(MakeStableID(3, 1, TileID{ 5, 0, 6 }, 0, 0));
	ids.push_back(MakeStableID(3, 1, TileID{ 5, 0, 6 }, 0, 1));
	TileID t{ 5, 0, 6 };
	auto blob = SerializeCache(0xAAAA, 0xBBBB, t, inst, ids);
	Check(!blob.empty(), "place.cache.serialize");
	std::vector<RenderInstance> out;
	std::vector<StableID> outIds;
	Check(DeserializeCache(blob.data(), blob.size(), 0xAAAA, 0xBBBB, t, out, outIds),
		"place.cache.deserialize-ok");
	Check(out.size() == 2 && out[0].position == glm::vec3(1, 2, 3)
		&& out[0].prototypeId == 4 && outIds[0] == ids[0], "place.cache.bit-roundtrip");
	// Stale keys rejected: wrong graph hash, resource hash, tile, truncation.
	Check(!DeserializeCache(blob.data(), blob.size(), 0xAAAB, 0xBBBB, t, out, outIds),
		"place.cache.graph-mismatch");
	Check(!DeserializeCache(blob.data(), blob.size(), 0xAAAA, 0xBBBC, t, out, outIds),
		"place.cache.resource-mismatch");
	Check(!DeserializeCache(blob.data(), blob.size(), 0xAAAA, 0xBBBB, TileID{ 0, 0, 0 }, out, outIds),
		"place.cache.tile-mismatch");
	Check(!DeserializeCache(blob.data(), blob.size() - 1, 0xAAAA, 0xBBBB, t, out, outIds),
		"place.cache.truncation-rejected");
	Check(!DeserializeCache(nullptr, 0, 0xAAAA, 0xBBBB, t, out, outIds),
		"place.cache.null-rejected");
	uint64_t k = HashCombine64(HashCombine64(0, 0xAAAA), 0xBBBB);
	Check(k != 0, "place.cache.key-combines");
}

void TestPhysicsQuery() {
	using namespace smvk::placement;
	PrototypeTable protos;
	Prototype tree; tree.assetUri = "tree"; tree.collisionRadius = 1.0f;
	protos.add(tree);
	std::vector<RenderInstance> inst(2);
	inst[0].position = glm::vec3(0, 0, 10); inst[0].scale = glm::vec3(1);
	inst[1].position = glm::vec3(0, 0, 20); inst[1].scale = glm::vec3(1);
	std::vector<StableID> ids = { MakeStableID(4, 0, TileID{}, 0, 0),
		MakeStableID(4, 0, TileID{}, 0, 1) };
	PlacementQuerySource src{ &inst, &ids, &protos, 0.5f };
	auto hit = RaycastPlacements(src, glm::vec3(0, 0, 0), glm::vec3(0, 0, 1), 100.0f);
	Check(hit.has_value(), "place.phys.ray-hit");
	if (hit) {
		Check(hit->id == ids[0], "place.phys.ray-nearest");
		Check(hit->distance < 10.0f && hit->distance > 8.0f, "place.phys.ray-t");
	}
	auto miss = RaycastPlacements(src, glm::vec3(0, 0, 0), glm::vec3(0, 1, 0), 100.0f);
	Check(!miss.has_value(), "place.phys.ray-miss");
	auto box = QueryPlacementsAABB(src, glm::vec3(-2, -2, 8), glm::vec3(2, 2, 12));
	Check(box.size() == 1 && box[0].id == ids[0], "place.phys.aabb-one");
	auto boxAll = QueryPlacementsAABB(src, glm::vec3(-50, -50, -50), glm::vec3(50, 50, 50));
	Check(boxAll.size() == 2, "place.phys.aabb-all");
}

void TestPromotion() {
	using namespace smvk::placement;
	PromotionRegistry reg;
	StableID id = MakeStableID(5, 0, TileID{}, 0, 9);
	Check(!reg.isSuppressed(id), "place.promo.clean");
	Check(reg.promote(id, 1001) == 1001, "place.promo.promote");
	Check(reg.isSuppressed(id), "place.promo.suppressed");
	Check(reg.promote(id, 9999) == 1001, "place.promo.idempotent");
	Check(reg.entityFor(id).value() == 1001, "place.promo.lookup");
	Check(reg.promotedCount() == 1, "place.promo.count");
	Check(reg.demoteByEntity(1001), "place.promo.demote-entity");
	Check(!reg.isSuppressed(id), "place.promo.restored");
	Check(!reg.demote(id), "place.promo.double-demote-false");
	reg.promote(id, 7);
	reg.clear();
	Check(reg.promotedCount() == 0, "place.promo.clear");
}

int main() {
	TestStableDeterminism();
	TestChannels();
	TestPrototypeSelect();
	TestPointsSoA();
	TestRenderInstanceAbi();
	TestGridDomain();
	TestMeshAlias();
	TestHeightfield();
	TestFieldEval();
	TestAcceptance();
	TestNoise();
	TestTileDeterminism();
	TestTileStreamingOrder();
	TestMinDistance();
	TestTileCache();
	TestTransientDraws();
	TestGpuPushAbi();
	TestOverrides();
	TestExclusions();
	TestCacheRoundTrip();
	TestPhysicsQuery();
	TestPromotion();
	std::cout << (g_failures == 0 ? "ALL PASS " : "FAILURES ") << g_checks << " checks\n";
	return g_failures == 0 ? 0 : 1;
}
