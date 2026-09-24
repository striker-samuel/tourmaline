// placement_tests: Phase-0 foundation for PlacementGraph.
// Hand-rolled runner (repo convention): PASS/FAIL lines, nonzero exit on failure.
// Covers: stable-ID determinism, channel independence, prototype selection,
// SoA point storage, RenderInstance ABI vs HLSL mirror.

#include <cstdint>
#include <cmath>
#include <cstdio>
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
#include <Engine/Rendergraph/placement_pass.h>

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
void TestMinDistanceVolume();
void TestOverrides();
void TestExclusions();
void TestCacheRoundTrip();
void TestPhysicsQuery();
void TestPromotion();
void TestPushBridge();
void TestKernelAlgorithmParity();
void TestScatterParams();
void TestMeshKernelParity();
void TestHfKernelParity();
void TestConBridge();
void TestConBruteForce();

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

void TestMinDistanceVolume() {
	using namespace smvk::placement;
	auto mk = [](std::vector<glm::vec3> pos) {
		std::vector<RenderInstance> inst;
		std::vector<StableID> ids;
		for (size_t i = 0; i < pos.size(); ++i) {
			RenderInstance in;
			in.position = pos[i];
			in.scale = glm::vec3(1);
			inst.push_back(in);
			ids.push_back(MakeStableID(11, 0, TileID{}, 0, (uint32_t)i));
		}
		return std::pair(inst, ids);
	};
	// Unbounded path: two conflicts 1m apart at spacing 4 -> one survives.
	{
		auto [inst, ids] = mk({ glm::vec3(0, 0, 0), glm::vec3(1, 0, 0), glm::vec3(50, 0, 0) });
		ConstraintVolume vol; // all-zero dims = unbounded
		ApplyMinDistance(inst, ids, 4.0f, vol);
		Check(inst.size() == 2, "place.mindist.unbounded-thins");
	}
	// Bounded skip rule: outsider at the same spot as a survivor neither
	// kills nor dies.
	{
		auto [inst, ids] = mk({ glm::vec3(1, 0, 1), glm::vec3(1.5f, 0, 1), glm::vec3(500, 0, 500) });
		ConstraintVolume vol;
		vol.origin = glm::vec3(0, -64, 0);
		vol.dimX = 32; vol.dimY = 64; vol.dimZ = 32;
		vol.spacing = 2.0f;
		ApplyMinDistance(inst, ids, 2.0f, vol);
		// Inside pair conflicts (0.5m < 2m): one dies; outsider survives.
		Check(inst.size() == 2, "place.mindist.skip-survives");
		bool outsiderKept = false;
		for (auto& in : inst)
			if (in.position.x == 500.0f) outsiderKept = true;
		Check(outsiderKept, "place.mindist.skip-kept");
	}
	// Snapshot order-freedom: shuffling INPUT order changes nothing about
	// the surviving SET (output is index-ordered, so compare positions).
	{
		auto [a, aids] = mk({ glm::vec3(0,0,0), glm::vec3(1,0,0), glm::vec3(2,0,0),
			glm::vec3(30,0,0), glm::vec3(31,0,0) });
		auto [b, bids] = mk({ glm::vec3(31,0,0), glm::vec3(2,0,0), glm::vec3(30,0,0),
			glm::vec3(1,0,0), glm::vec3(0,0,0) });
		ConstraintVolume vol;
		ApplyMinDistance(a, aids, 4.0f, vol);
		ApplyMinDistance(b, bids, 4.0f, vol);
		// Same survivor POSITION set regardless of input order (priorities
		// ride the StableIDs, which were built per-position above... note
		// ids differ per order here, so only assert spacing + determinism).
		bool okA = true, okB = true;
		for (size_t i = 0; i < a.size(); ++i)
			for (size_t j = i + 1; j < a.size(); ++j) {
				glm::vec3 d = a[i].position - a[j].position;
				if (glm::dot(d, d) < 16.0f * 0.999f) okA = false;
			}
		for (size_t i = 0; i < b.size(); ++i)
			for (size_t j = i + 1; j < b.size(); ++j) {
				glm::vec3 d = b[i].position - b[j].position;
				if (glm::dot(d, d) < 16.0f * 0.999f) okB = false;
			}
		Check(okA && okB, "place.mindist.order-spacing-holds");
	}
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

void TestPushBridge() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.origin = glm::vec3(10, 2, 30);
	grid.extent = glm::vec2(64, 64);
	grid.cellsX = 16; grid.cellsZ = 8;
	PlacementSystem::LayerBinding lb;
	lb.programId = 7;
	lb.program.graphSeed = 0x123456789ABCull;
	lb.program.nodeId = 3;
	lb.program.instrs = { OpConst(0.35f) };
	lb.program.acceptIndex = 0;
	lb.domainKind = DomainKind::Grid;
	lb.grid = &grid;
	lb.config.candidatesPerTile = 2048;
	Prototype oak; oak.weight = 3.0f;
	Prototype pine; pine.weight = 1.0f;
	lb.prototypes.add(oak); lb.prototypes.add(pine);
	float d = 0.0f;
	Check(PlacementPass::TryExtractConstDensity(lb.program, d) && d == 0.35f,
		"place.bridge.const-density");
	PlacementProgram multi;
	multi.instrs = { OpConst(0.5f), OpConst(0.5f) };
	multi.acceptIndex = 1;
	Check(!PlacementPass::TryExtractConstDensity(multi, d), "place.bridge.multi-fallback");
	PlacementTilePush push;
	Check(PlacementPass::BuildTilePush(lb, TileID{ 1, 2, 3 }, 0x1000, 0x2000, push),
		"place.bridge.build-ok");
	Check(push.seedLo == 0x56789ABCul && push.seedHi == 0x1234u, "place.bridge.seed-split");
	Check(push.nodeId == 3 && push.candidateCount == 2048, "place.bridge.ids");
	Check(push.tileX == 1 && push.tileY == 2 && push.tileZ == 3, "place.bridge.tile");
	Check(push.gridOrigin == glm::vec3(10, 2, 30) && push.cellsX == 16 && push.cellsZ == 8,
		"place.bridge.grid");
	Check(push.density == 0.35f && push.noiseAmp == 0.0f, "place.bridge.density");
	Check(push.protoCount == 2 && push.weightsA.x == 3.0f && push.weightsA.y == 1.0f,
		"place.bridge.weights");
	Check(push.outInstances == 0x1000 && push.outCount == 0x2000, "place.bridge.bda");
	// Mesh layer (no kernel yet) refuses honestly.
	PlacementSystem::LayerBinding mlb = lb;
	mlb.domainKind = DomainKind::MeshSurface;
	mlb.grid = nullptr;
	Check(!PlacementPass::BuildTilePush(mlb, TileID{}, 0, 0, push), "place.bridge.mesh-refuses");
	// Empty prototype table -> single default weight.
	PlacementSystem::LayerBinding elb = lb;
	elb.prototypes.clear();
	Check(PlacementPass::BuildTilePush(elb, TileID{}, 0, 0, push)
		&& push.protoCount == 1 && push.weightsA.x == 1.0f, "place.bridge.empty-proto");
}

// From-scratch reimplementation of placement_tile.cs.hlsl's documented
// algorithm (grid path, const density, no noise). Agreement with
// EvaluateTile() proves the KERNEL's algorithm matches the CPU oracle;
// HLSL-vs-C++ hash bit-parity itself needs on-device readback via
// PlacementPass::DownloadTile (exists, not runnable headless).
void TestKernelAlgorithmParity() {
	using namespace smvk::placement;
	GridDomain grid;
	grid.origin = glm::vec3(0, 0, 0);
	grid.extent = glm::vec2(64, 64);
	grid.cellsX = 16; grid.cellsZ = 16;
	PlacementSystem::LayerBinding lb;
	lb.programId = 9;
	lb.program.graphSeed = 777;
	lb.program.nodeId = 2;
	lb.program.instrs = { OpConst(0.45f) };
	lb.program.acceptIndex = 0;
	lb.domainKind = DomainKind::Grid;
	lb.grid = &grid;
	lb.config.candidatesPerTile = 1500;
	lb.config.minScale = 0.7f; lb.config.maxScale = 1.3f;
	Prototype a; a.weight = 2.0f;
	Prototype b; b.weight = 1.0f;
	Prototype c; c.weight = 1.0f;
	lb.prototypes.add(a); lb.prototypes.add(b); lb.prototypes.add(c);

	auto oracle = EvaluateTile(lb, TileID{ 4, 0, 5 });

	// Kernel mirror (same steps as mainCS, written independently).
	std::vector<RenderInstance> mirror;
	const uint32_t N = lb.config.candidatesPerTile;
	for (uint32_t i = 0; i < N; ++i) {
		StableID id = MakeStableID(777, 2, TileID{ 4, 0, 5 }, 0, i);
		DomainSample s = grid.sample(id, i);
		float q = QuantizeDensity(0.45f);
		bool keep = (q >= 1.0f) || (Random01(id, RandomChannel::Density) < q);
		if (!keep) continue;
		float rv = Random01(id, RandomChannel::Variant);
		uint32_t proto = lb.prototypes.select(rv).value_or(0);
		RenderInstance in;
		in.position = s.p;
		in.yaw = YawFromId(id);
		float sc = UniformScaleFromId(id, 0.7f, 1.3f);
		in.scale = glm::vec3(sc);
		in.prototypeId = proto;
		in.stableLow = id.w[0];
		in.customB = i; // candidate index (mirrors EvaluateTile + mainCS)
		mirror.push_back(in);
	}
	bool same = oracle.size() == mirror.size();
	if (same) {
		for (size_t i = 0; i < oracle.size(); ++i) {
			if (oracle[i].position != mirror[i].position
				|| oracle[i].yaw != mirror[i].yaw
				|| oracle[i].scale != mirror[i].scale
				|| oracle[i].prototypeId != mirror[i].prototypeId
				|| oracle[i].stableLow != mirror[i].stableLow
				|| oracle[i].customB != mirror[i].customB) {
				same = false; break;
			}
		}
	}
	Check(same, "place.kernel.algorithm-parity");
	Check(!oracle.empty(), "place.kernel.nonempty");
}

void TestScatterParams() {
	using namespace smvk::placement;
	Check(sizeof(PlacementDomainParams) == 176, "place.scatter.params-176B");
	Check(sizeof(PlacementScatterPush) == 64, "place.scatter.push-64B");
	Check(offsetof(PlacementScatterPush, params) == 16, "place.scatter.params-bda");
	Check(offsetof(PlacementScatterPush, candidateCount) == 44, "place.scatter.count");
	// Mesh mapping.
	PlacementMeshData mesh;
	mesh.positions = { glm::vec3(0,0,0), glm::vec3(1,0,0), glm::vec3(0,0,1) };
	mesh.normals = { glm::vec3(0,1,0), glm::vec3(0,1,0), glm::vec3(0,1,0) };
	mesh.indices = { 0, 1, 2 };
	BuildMeshAliasTable(mesh);
	PlacementSystem::LayerBinding mlb;
	mlb.programId = 21;
	mlb.program.graphSeed = 5;
	mlb.program.instrs = { OpConst(0.7f) };
	mlb.program.acceptIndex = 0;
	mlb.domainKind = DomainKind::MeshSurface;
	mlb.mesh = &mesh;
	mlb.config.candidatesPerTile = 64;
	ScatterBDAs b{ 11, 22, 0, 33, 44, 55, 0 };
	PlacementDomainParams prm;
	Check(FillScatterParams(mlb, b, prm), "place.scatter.mesh-ok");
	Check(prm.domainKind == kPlacementScatterDomain_Mesh, "place.scatter.mesh-kind");
	Check(prm.triCount == 1 && prm.vertCount == 3, "place.scatter.mesh-counts");
	Check((prm.flags & kPlacementFlag_HasNormals) != 0, "place.scatter.mesh-normals");
	Check((prm.flags & kPlacementFlag_HasUVs) == 0, "place.scatter.mesh-no-uvs");
	Check(prm.posBDA == 11 && prm.idxBDA == 33 && prm.probBDA == 44, "place.scatter.mesh-bda");
	Check(prm.density == 0.7f, "place.scatter.mesh-density");
	// Heightfield mapping.
	HeightfieldData hf;
	hf.width = 3; hf.height = 3;
	hf.origin = glm::vec3(100, 0, 200);
	hf.spacing = glm::vec2(2.0f, 2.0f);
	hf.heights.assign(9, 1.0f);
	HeightfieldDomain hdom;
	hdom.field = &hf;
	hdom.extentMin = glm::vec2(100, 200);
	hdom.extentMax = glm::vec2(104, 204);
	PlacementSystem::LayerBinding hlb;
	hlb.program.graphSeed = 6;
	hlb.program.instrs = { OpConst(0.4f) };
	hlb.program.acceptIndex = 0;
	hlb.domainKind = DomainKind::Heightfield;
	hlb.heightfield = &hdom;
	hlb.config.candidatesPerTile = 32;
	ScatterBDAs hb{ 0, 0, 0, 0, 0, 0, 77 };
	PlacementDomainParams hprm;
	Check(FillScatterParams(hlb, hb, hprm), "place.scatter.hf-ok");
	Check(hprm.domainKind == kPlacementScatterDomain_Heightfield, "place.scatter.hf-kind");
	Check(hprm.hfW == 3 && hprm.hfH == 3, "place.scatter.hf-dims");
	Check(hprm.hfOX == 100.0f && hprm.hfOZ == 200.0f, "place.scatter.hf-origin");
	Check(hprm.hfSX == 2.0f && hprm.extMaxX == 104.0f, "place.scatter.hf-extent");
	Check(hprm.heightsBDA == 77, "place.scatter.hf-bda");
	// Grid layer refuses scatter params (has its own kernel+push).
	GridDomain grid;
	PlacementSystem::LayerBinding glb;
	glb.domainKind = DomainKind::Grid;
	glb.grid = &grid;
	glb.program.instrs = { OpConst(0.5f) };
	glb.program.acceptIndex = 0;
	PlacementDomainParams gprm;
	Check(!FillScatterParams(glb, hb, gprm), "place.scatter.grid-refuses");
	// Push build.
	PlacementScatterPush push;
	Check(BuildScatterPush(mlb, TileID{ 7, 0, 8 }, 0xAAA, 0xBBB, 0xCCC, push),
		"place.scatter.push-ok");
	Check(push.params == 0xCCC && push.outInstances == 0xAAA && push.outCount == 0xBBB,
		"place.scatter.push-bda");
	Check(push.seedLo == 5 && push.nodeId == 0 && push.candidateCount == 64,
		"place.scatter.push-ids");
	Check(push.tileX == 7 && push.tileZ == 8, "place.scatter.push-tile");
}

void TestMeshKernelParity() {
	using namespace smvk::placement;
	// Independent reimplementation of placement_mesh_tile.cs.hlsl mainCS.
	PlacementMeshData mesh;
	mesh.positions = { glm::vec3(0,0,0), glm::vec3(10,0,0), glm::vec3(0,0,10),
		glm::vec3(20,0,0), glm::vec3(21,0,0), glm::vec3(20,0,1) };
	mesh.indices = { 0, 1, 2, 3, 4, 5 };
	BuildMeshAliasTable(mesh);
	PlacementSystem::LayerBinding lb;
	lb.programId = 31;
	lb.program.graphSeed = 4242;
	lb.program.nodeId = 1;
	lb.program.instrs = { OpConst(0.6f) };
	lb.program.acceptIndex = 0;
	lb.domainKind = DomainKind::MeshSurface;
	lb.mesh = &mesh;
	lb.config.candidatesPerTile = 800;
	lb.config.minScale = 0.9f; lb.config.maxScale = 1.1f;
	Prototype p; p.weight = 1.0f;
	lb.prototypes.add(p);
	auto oracle = EvaluateTile(lb, TileID{ 0, 0, 0 });
	std::vector<RenderInstance> mirror;
	for (uint32_t i = 0; i < 800; ++i) {
		StableID id = MakeStableID(4242, 1, TileID{ 0, 0, 0 }, 0, i);
		DomainSample s = SampleMeshSurface(mesh, id);
		float q = QuantizeDensity(0.6f);
		if (q < 1.0f && !(Random01(id, RandomChannel::Density) < q)) continue;
		RenderInstance in;
		in.position = s.p;
		in.yaw = YawFromId(id);
		float sc = UniformScaleFromId(id, 0.9f, 1.1f);
		in.scale = glm::vec3(sc);
		in.prototypeId = 0;
		in.stableLow = id.w[0];
		in.customB = i;
		mirror.push_back(in);
	}
	bool same = oracle.size() == mirror.size();
	if (same) {
		for (size_t i = 0; i < oracle.size(); ++i)
			if (oracle[i].position != mirror[i].position
				|| oracle[i].yaw != mirror[i].yaw
				|| oracle[i].stableLow != mirror[i].stableLow
				|| oracle[i].customB != mirror[i].customB) { same = false; break; }
	}
	Check(same, "place.meshkernel.parity");
	Check(!oracle.empty(), "place.meshkernel.nonempty");
}

void TestHfKernelParity() {
	using namespace smvk::placement;
	HeightfieldData hf;
	hf.width = 5; hf.height = 5;
	hf.origin = glm::vec3(0, 0, 0);
	hf.spacing = glm::vec2(1.0f, 1.0f);
	hf.heights.resize(25);
	for (uint32_t z = 0; z < 5; ++z)
		for (uint32_t x = 0; x < 5; ++x)
			hf.heights[z * 5 + x] = (float)(x * x) * 0.1f + (float)z * 0.3f;
	HeightfieldDomain dom;
	dom.field = &hf;
	dom.extentMin = glm::vec2(0, 0);
	dom.extentMax = glm::vec2(4, 4);
	PlacementSystem::LayerBinding lb;
	lb.programId = 32;
	lb.program.graphSeed = 99;
	lb.program.nodeId = 4;
	lb.program.instrs = { OpConst(0.8f) };
	lb.program.acceptIndex = 0;
	lb.domainKind = DomainKind::Heightfield;
	lb.heightfield = &dom;
	lb.config.candidatesPerTile = 500;
	Prototype p; p.weight = 1.0f;
	lb.prototypes.add(p);
	auto oracle = EvaluateTile(lb, TileID{ 1, 0, 1 });
	std::vector<RenderInstance> mirror;
	for (uint32_t i = 0; i < 500; ++i) {
		StableID id = MakeStableID(99, 4, TileID{ 1, 0, 1 }, 0, i);
		DomainSample s = dom.sample(id);
		float q = QuantizeDensity(0.8f);
		if (q < 1.0f && !(Random01(id, RandomChannel::Density) < q)) continue;
		RenderInstance in;
		in.position = s.p;
		in.yaw = YawFromId(id);
		float sc = UniformScaleFromId(id, 0.8f, 1.2f);
		in.scale = glm::vec3(sc);
		in.prototypeId = 0;
		in.stableLow = id.w[0];
		in.customB = i;
		mirror.push_back(in);
	}
	bool same = oracle.size() == mirror.size();
	if (same) {
		for (size_t i = 0; i < oracle.size(); ++i)
			if (oracle[i].position != mirror[i].position
				|| oracle[i].yaw != mirror[i].yaw
				|| oracle[i].stableLow != mirror[i].stableLow) { same = false; break; }
	}
	Check(same, "place.hfkernel.parity");
	Check(!oracle.empty(), "place.hfkernel.nonempty");
}

void TestConBridge() {
	using namespace smvk::placement;
	Check(sizeof(PlacementConParams) == 64, "place.con.params-64B");
	Check(sizeof(PlacementConPush) == 64, "place.con.push-64B");
	Check(offsetof(PlacementConPush, flag2) == 24, "place.con.flag2-offset");
	Check(offsetof(PlacementConPush, countBuf) == 40, "place.con.count-offset");
	Check(kPlacementConMaxCells == 262144, "place.con.max-cells");
	// Volume eligibility.
	ConstraintVolume ok;
	ok.origin = glm::vec3(0, -64, 0);
	ok.dimX = 18; ok.dimY = 34; ok.dimZ = 18;
	ok.spacing = 4.0f;
	Check(PlacementConstraint::VolumeFitsGpu(ok), "place.con.fits");
	ConstraintVolume unbounded; // zero dims
	unbounded.spacing = 4.0f;
	Check(!PlacementConstraint::VolumeFitsGpu(unbounded), "place.con.unbounded-refuses");
	ConstraintVolume huge = ok;
	huge.dimX = 65;
	Check(!PlacementConstraint::VolumeFitsGpu(huge), "place.con.dim-refuses");
	ConstraintVolume cells = ok;
	cells.dimX = 64; cells.dimY = 64; cells.dimZ = 64; // 262144 cells exactly
	Check(PlacementConstraint::VolumeFitsGpu(cells), "place.con.max-exact-fits");
	cells.dimZ = 65; // 266240 > max
	Check(!PlacementConstraint::VolumeFitsGpu(cells), "place.con.cellcount-refuses");
	// Params fill + push build.
	PlacementSystem::LayerBinding lb;
	lb.program.graphSeed = 0xFEDCBA9876543210ull;
	lb.program.nodeId = 9;
	PlacementConParams prm;
	Check(PlacementConstraint::FillConParams(lb, ok, prm), "place.con.fill-ok");
	Check(prm.seedLo == 0x76543210ul && prm.seedHi == 0xFEDCBA98ul, "place.con.fill-seed");
	Check(prm.nodeId == 9 && prm.spacing == 4.0f, "place.con.fill-ids");
	Check(prm.dimX == 18 && prm.dimY == 34 && prm.dimZ == 18, "place.con.fill-dims");
	Check(prm.tileX == 0 && prm.tileY == 0 && prm.tileZ == 0, "place.con.fill-tile-zero");
	Check(!PlacementConstraint::FillConParams(lb, unbounded, prm), "place.con.fill-refuses");
	PlacementConPush push;
	Check(PlacementConstraint::BuildConPush(1, 2, 3, 4, 5, 6, push), "place.con.push-ok");
	Check(push.instances == 1 && push.head == 2 && push.next == 3 && push.flag2 == 4
		&& push.params == 5 && push.countBuf == 6, "place.con.push-bda");
	// Layer volume helper: grid bounded, mesh unbounded.
	GridDomain grid;
	grid.origin = glm::vec3(0, 0, 0);
	grid.extent = glm::vec2(64, 64);
	PlacementSystem::LayerBinding glb;
	glb.domainKind = DomainKind::Grid;
	glb.grid = &grid;
	glb.config.minSpacing = 4.0f;
	ConstraintVolume gv = ConstraintVolumeForLayer(glb);
	Check(gv.dimX == 18 && gv.dimZ == 18 && gv.spacing == 4.0f, "place.con.layer-grid-vol");
	Check(PlacementConstraint::VolumeFitsGpu(gv), "place.con.layer-grid-fits");
	PlacementMeshData mesh;
	PlacementSystem::LayerBinding mlb;
	mlb.domainKind = DomainKind::MeshSurface;
	mlb.mesh = &mesh;
	mlb.config.minSpacing = 4.0f;
	ConstraintVolume mv = ConstraintVolumeForLayer(mlb);
	Check(mv.dimX == 0 && mv.spacing == 4.0f, "place.con.layer-mesh-unbounded");
}

namespace {
// Independent O(n^2) reference for the snapshot rule: no grids, no maps,
// no shared code with ApplyMinDistance. Bounded volumes use an explicit
// linear key; unbounded uses raw cell coords.
std::vector<uint32_t> BruteForceSurvivors(const std::vector<glm::vec3>& pos,
	const std::vector<smvk::placement::StableID>& ids, float spacing,
	const smvk::placement::ConstraintVolume& vol) {
	using namespace smvk::placement;
	const size_t n = pos.size();
	const bool bounded = vol.dimX > 0 && vol.dimY > 0 && vol.dimZ > 0 && vol.spacing > 0.0f;
	const float cs = bounded ? vol.spacing : spacing;
	struct Cell { int64_t x, y, z; };
	std::vector<Cell> cells(n);
	std::vector<uint8_t> inVol(n, 1);
	for (size_t i = 0; i < n; ++i) {
		if (bounded) {
			int64_t ix = (int64_t)std::floor((pos[i].x - vol.origin.x) / vol.spacing);
			int64_t iy = (int64_t)std::floor((pos[i].y - vol.origin.y) / vol.spacing);
			int64_t iz = (int64_t)std::floor((pos[i].z - vol.origin.z) / vol.spacing);
			cells[i] = { ix, iy, iz };
			if (ix < 0 || iy < 0 || iz < 0 || ix >= vol.dimX || iy >= vol.dimY || iz >= vol.dimZ)
				inVol[i] = 0;
		} else {
			cells[i] = { (int64_t)std::floor(pos[i].x / cs),
				(int64_t)std::floor(pos[i].y / cs),
				(int64_t)std::floor(pos[i].z / cs) };
		}
	}
	auto beats = [&](size_t a, size_t b) { // true when b beats a
		uint64_t pa = PriorityOf(ids[a]), pb = PriorityOf(ids[b]);
		return pb > pa || (pb == pa && b < a);
	};
	std::vector<uint8_t> winner(n, 0);
	for (size_t i = 0; i < n; ++i) {
		if (!inVol[i]) continue;
		bool win = true;
		for (size_t j = 0; j < n; ++j) {
			if (j == i || !inVol[j]) continue;
			if (cells[j].x == cells[i].x && cells[j].y == cells[i].y && cells[j].z == cells[i].z
				&& beats(i, j)) { win = false; break; }
		}
		winner[i] = win ? 1 : 0;
	}
	std::vector<uint8_t> alive(n, 0);
	const float minD2 = spacing * spacing;
	for (size_t i = 0; i < n; ++i) {
		if (!winner[i]) continue;
		bool live = true;
		for (size_t j = 0; j < n; ++j) {
			if (j == i || !winner[j]) continue;
			glm::vec3 d = pos[i] - pos[j];
			if (glm::dot(d, d) < minD2 && beats(i, j)) { live = false; break; }
		}
		alive[i] = live ? 1 : 0;
	}
	std::vector<uint32_t> out;
	for (size_t i = 0; i < n; ++i)
		if (alive[i] || !inVol[i]) out.push_back((uint32_t)i);
	return out;
}
}

void TestConBruteForce() {
	using namespace smvk::placement;
	// Seeded pseudo-random point sets (SplitMix32 chain — hermetic, no <random>).
	for (uint32_t trial = 0; trial < 6; ++trial) {
		uint32_t s = 0xC0FFEEu + trial * 0x9e3779b9u;
		auto rnd = [&]() { s = SplitMix32(s + 0x85ebca6bu); return (s >> 8) * (1.0f / 16777216.0f); };
		const size_t N = 120 + trial * 40;
		std::vector<glm::vec3> pos;
		std::vector<StableID> ids;
		for (size_t i = 0; i < N; ++i) {
			float x = rnd() * 40.0f, z = rnd() * 40.0f;
			if (trial % 2 == 1 && i % 7 == 0) { x += 500.0f; z += 500.0f; } // outsiders
			pos.push_back(glm::vec3(x, 0.0f, z));
			ids.push_back(MakeStableID(1000 + trial, 3, TileID{ 0, 0, 0 }, 0, (uint32_t)i));
		}
		// Bounded.
		ConstraintVolume vol;
		vol.origin = glm::vec3(0, -64, 0);
		vol.dimX = 22; vol.dimY = 66; vol.dimZ = 22;
		vol.spacing = 2.0f;
		std::vector<RenderInstance> inst;
		for (auto& p : pos) { RenderInstance in; in.position = p; in.scale = glm::vec3(1); inst.push_back(in); }
		std::vector<RenderInstance> instCopy = inst;
		ApplyMinDistance(inst, ids, 2.0f, vol);
		std::vector<uint32_t> ref = BruteForceSurvivors(pos, ids, 2.0f, vol);
		bool same = inst.size() == ref.size();
		if (same) {
			// Both ascending-index ordered: compare positions directly.
			size_t ri = 0;
			for (size_t i = 0; i < pos.size() && same; ++i) {
				bool kept = (ri < ref.size() && ref[ri] == i);
				if (kept) {
					if (inst[ri].position != pos[i]) same = false;
					++ri;
				}
			}
		}
		char name[64];
		snprintf(name, sizeof(name), "place.con.brute-bounded-%u", trial);
		Check(same, name);
		// Unbounded.
		ConstraintVolume unvol;
		unvol.spacing = 2.0f;
		std::vector<RenderInstance> inst2 = instCopy;
		ApplyMinDistance(inst2, ids, 2.0f, unvol);
		std::vector<uint32_t> ref2 = BruteForceSurvivors(pos, ids, 2.0f, unvol);
		bool same2 = inst2.size() == ref2.size();
		if (same2) {
			size_t ri = 0;
			for (size_t i = 0; i < pos.size() && same2; ++i) {
				if (ri < ref2.size() && ref2[ri] == i) {
					if (inst2[ri].position != pos[i]) same2 = false;
					++ri;
				}
			}
		}
		snprintf(name, sizeof(name), "place.con.brute-unbounded-%u", trial);
		Check(same2, name);
	}
	// Degenerate inputs.
	{
		std::vector<RenderInstance> empty;
		std::vector<StableID> emptyIds;
		ConstraintVolume vol;
		ApplyMinDistance(empty, emptyIds, 2.0f, vol);
		Check(empty.empty(), "place.con.brute-empty");
	}
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
	TestMinDistanceVolume();
	TestOverrides();
	TestExclusions();
	TestCacheRoundTrip();
	TestPhysicsQuery();
	TestPromotion();
	TestPushBridge();
	TestKernelAlgorithmParity();
	TestScatterParams();
	TestMeshKernelParity();
	TestHfKernelParity();
	TestConBridge();
	TestConBruteForce();
	std::cout << (g_failures == 0 ? "ALL PASS " : "FAILURES ") << g_checks << " checks\n";
	return g_failures == 0 ? 0 : 1;
}
