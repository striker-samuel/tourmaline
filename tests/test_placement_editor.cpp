// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// placement_editor_tests: headless tests for the placement graph editor
// model (editor/src/editor_placement.cpp — engine-only TU, no ImGui).
// Covers: scaffold validity, compile, serialize round-trip, link rules,
// cycle detection, error cases, eval sanity, undo/redo.
//
// Hand-rolled runner (repo convention): PASS/FAIL lines, nonzero exit.

#include <cmath>
#include <iostream>
#include <string>

#include "editor_placement.h"

namespace {
int g_failures = 0;
int g_checks = 0;
void Check(bool cond, const char* name) {
	++g_checks;
	if (cond) std::cout << "PASS " << name << "\n";
	else { ++g_failures; std::cout << "FAIL " << name << "\n"; }
}
using namespace editor::placement;
using namespace smvk::placement;
} // namespace

void TestScaffoldCompiles() {
	GraphDoc doc;
	BuildDefaultGraph(doc);
	Check(doc.nodes.size() == 7, "pe.scaffold.node-count");
	CompileResult cr = CompileGraph(doc, 1);
	Check(cr.ok, "pe.scaffold.compiles");
	if (!cr.ok) std::cout << "  err: " << cr.error << "\n";
	Check(cr.program.valid(), "pe.scaffold.program-valid");
	Check(cr.gpuOk, "pe.scaffold.gpu-ok");
	Check(cr.prototypes.size() == 2, "pe.scaffold.two-protos");
	// Default program at a flat sample: Density 0.5 x scale 1.
	DomainSample s;
	s.p = glm::vec3(0, 0, 0);
	s.n = glm::vec3(0, 1, 0);
	std::vector<float> v = EvaluateProgram(cr.program, s);
	Check(std::abs(v[cr.program.acceptIndex] - 0.5f) < 1e-5f,
		"pe.scaffold.density-half");
}

void TestAcceptScale() {
	GraphDoc doc;
	BuildDefaultGraph(doc);
	for (auto& [id, n] : doc.nodes)
		if (n.kind == NodeKind::Accept) n.f[0] = 2.0f;
	CompileResult cr = CompileGraph(doc, 1);
	Check(cr.ok, "pe.scale.compiles");
	DomainSample s;
	s.n = glm::vec3(0, 1, 0);
	std::vector<float> v = EvaluateProgram(cr.program, s);
	Check(std::abs(v[cr.program.acceptIndex] - 1.0f) < 1e-5f,
		"pe.scale.folds");
}

void TestFieldChain() {
	// Density(0.25) -> Mul(Smoothstep(Noise)) -> Accept; hand-check eval.
	GraphDoc doc;
	const uint32_t grid = doc.addNode(NodeKind::Grid, 0, 0);
	const uint32_t dens = doc.addNode(NodeKind::Density, 0, 0);
	doc.nodes[dens].f[0] = 0.25f;
	const uint32_t noise = doc.addNode(NodeKind::Noise, 0, 0);
	const uint32_t ss = doc.addNode(NodeKind::Smoothstep, 0, 0);
	const uint32_t mul = doc.addNode(NodeKind::Mul, 0, 0);
	const uint32_t acc = doc.addNode(NodeKind::Accept, 0, 0);
	const uint32_t layer = doc.addNode(NodeKind::Layer, 0, 0);
	const uint32_t protos = doc.addNode(NodeKind::Prototypes, 0, 0);
	Check(doc.addLink(grid, "domain", layer, "domain") != 0, "pe.chain.wire-domain");
	Check(doc.addLink(noise, "out", ss, "x") != 0, "pe.chain.wire-noise");
	Check(doc.addLink(dens, "out", mul, "a") != 0, "pe.chain.wire-a");
	Check(doc.addLink(ss, "out", mul, "b") != 0, "pe.chain.wire-b");
	Check(doc.addLink(mul, "out", acc, "density") != 0, "pe.chain.wire-acc");
	Check(doc.addLink(acc, "out", layer, "density") != 0, "pe.chain.wire-layer");
	Check(doc.addLink(protos, "protos", layer, "protos") != 0, "pe.chain.wire-protos");
	CompileResult cr = CompileGraph(doc, 7);
	Check(cr.ok, "pe.chain.compiles");
	if (!cr.ok) std::cout << "  err: " << cr.error << "\n";
	DomainSample s;
	s.p = glm::vec3(10, 0, 20);
	s.n = glm::vec3(0, 1, 0);
	std::vector<float> v = EvaluateProgram(cr.program, s);
	const float n = ValueNoise2D(10 * 0.02f, 20 * 0.02f, cr.program.graphSeed);
	const float ssv = n * n * (3.0f - 2.0f * n); // smoothstep(0,1,n)
	const float expect = 0.25f * ssv;
	Check(std::abs(v[cr.program.acceptIndex] - expect) < 1e-5f,
		"pe.chain.eval-matches");
	// instrToNode covers every instruction.
	Check(cr.instrToNode.size() == cr.program.instrs.size(),
		"pe.chain.trace-map");
}

void TestLinkRules() {
	GraphDoc doc;
	const uint32_t grid = doc.addNode(NodeKind::Grid, 0, 0);
	const uint32_t dens = doc.addNode(NodeKind::Density, 0, 0);
	const uint32_t acc = doc.addNode(NodeKind::Accept, 0, 0);
	const uint32_t layer = doc.addNode(NodeKind::Layer, 0, 0);
	// Type clash: domain -> field input.
	Check(doc.addLink(grid, "domain", acc, "density") == 0, "pe.link.type-clash");
	// Unknown pin.
	Check(doc.addLink(dens, "nope", acc, "density") == 0, "pe.link.bad-pin");
	// Self link.
	Check(doc.addLink(acc, "out", acc, "density") == 0, "pe.link.self");
	// Dangling node.
	Check(doc.addLink(9999, "out", acc, "density") == 0, "pe.link.dangling");
	// Valid link, then a second writer on the same input is rejected.
	Check(doc.addLink(dens, "out", acc, "density") != 0, "pe.link.ok");
	const uint32_t dens2 = doc.addNode(NodeKind::Density, 0, 0);
	Check(doc.addLink(dens2, "out", acc, "density") == 0, "pe.link.single-writer");
	// Unlinked field input errors at compile.
	GraphDoc doc2;
	const uint32_t g2 = doc2.addNode(NodeKind::Grid, 0, 0);
	const uint32_t m2 = doc2.addNode(NodeKind::Mul, 0, 0);
	const uint32_t a2 = doc2.addNode(NodeKind::Accept, 0, 0);
	const uint32_t l2 = doc2.addNode(NodeKind::Layer, 0, 0);
	doc2.addLink(g2, "domain", l2, "domain");
	doc2.addLink(m2, "out", a2, "density");
	doc2.addLink(a2, "out", l2, "density");
	CompileResult cr = CompileGraph(doc2, 1);
	Check(!cr.ok, "pe.link.missing-input-errors");
}

void TestCycle() {
	GraphDoc doc;
	const uint32_t grid = doc.addNode(NodeKind::Grid, 0, 0);
	const uint32_t a = doc.addNode(NodeKind::Add, 0, 0);
	const uint32_t b = doc.addNode(NodeKind::Add, 0, 0);
	const uint32_t acc = doc.addNode(NodeKind::Accept, 0, 0);
	const uint32_t layer = doc.addNode(NodeKind::Layer, 0, 0);
	doc.addLink(grid, "domain", layer, "domain");
	doc.addLink(a, "out", b, "a");
	doc.addLink(b, "out", a, "a"); // cycle (b also missing "b", cycle hits first)
	doc.addLink(a, "out", acc, "density");
	doc.addLink(acc, "out", layer, "density");
	CompileResult cr = CompileGraph(doc, 1);
	Check(!cr.ok, "pe.cycle.errors");
}

void TestErrors() {
	{ // no layer
		GraphDoc doc;
		doc.addNode(NodeKind::Grid, 0, 0);
		Check(!CompileGraph(doc, 1).ok, "pe.err.no-layer");
	}
	{ // two layers
		GraphDoc doc;
		BuildDefaultGraph(doc);
		doc.addNode(NodeKind::Layer, 900, 120);
		Check(!CompileGraph(doc, 1).ok, "pe.err.two-layers");
	}
	{ // unbound mesh
		GraphDoc doc;
		BuildDefaultGraph(doc);
		for (auto& [id, n] : doc.nodes)
			if (n.kind == NodeKind::Grid) {
				n.kind = NodeKind::Mesh;
				n.f.clear();
				n.i = { -1 };
			}
		// Rewire: mesh out pin is "domain", same as grid — links survive.
		CompileResult cr = CompileGraph(doc, 1);
		Check(!cr.ok, "pe.err.unbound-mesh");
	}
	{ // >8 prototypes
		GraphDoc doc;
		BuildDefaultGraph(doc);
		for (auto& [id, n] : doc.nodes)
			if (n.kind == NodeKind::Prototypes) {
				for (int k = 0; k < 8; ++k) {
					n.s.push_back("Extra" + std::to_string(k));
					n.f.push_back(1.0f);
					n.f.push_back(0.5f);
				}
			}
		Check(!CompileGraph(doc, 1).ok, "pe.err.too-many-protos");
	}
	{ // empty seed
		GraphDoc doc;
		BuildDefaultGraph(doc);
		for (auto& [id, n] : doc.nodes)
			if (n.kind == NodeKind::Layer) n.s[0].clear();
		Check(!CompileGraph(doc, 1).ok, "pe.err.empty-seed");
	}
}

void TestRoundTrip() {
	GraphDoc doc;
	BuildDefaultGraph(doc);
	// Exercise strings with spaces + non-default params.
	for (auto& [id, n] : doc.nodes) {
		if (n.kind == NodeKind::Prototypes) {
			n.s[0] = "Big Oak";
			n.f[2] = 2.5f;
		}
		if (n.kind == NodeKind::Grid) n.pos[0] = 33.5f;
		if (n.kind == NodeKind::Layer) n.s[0] = "987654321012345";
	}
	const std::string text = SerializeGraph(doc);
	GraphDoc back;
	Check(ParseGraph(text, back), "pe.rt.parses");
	Check(back.nodes.size() == doc.nodes.size(), "pe.rt.node-count");
	Check(back.links.size() == doc.links.size(), "pe.rt.link-count");
	bool same = back.name == doc.name && back.nextId == doc.nextId;
	for (const auto& [id, n] : doc.nodes) {
		const PNode* m = back.find(id);
		if (!m || m->kind != n.kind || m->f != n.f || m->i != n.i || m->s != n.s
			|| m->pos[0] != n.pos[0] || m->pos[1] != n.pos[1]) {
			same = false;
			break;
		}
	}
	for (const auto& [id, l] : doc.links) {
		auto it = back.links.find(id);
		if (it == back.links.end() || it->second.from != l.from
			|| it->second.fromPin != l.fromPin || it->second.to != l.to
			|| it->second.toPin != l.toPin) {
			same = false;
			break;
		}
	}
	Check(same, "pe.rt.identical");
	// Round-trip compiles identically.
	CompileResult a = CompileGraph(doc, 3);
	CompileResult b = CompileGraph(back, 3);
	Check(a.ok && b.ok, "pe.rt.both-compile");
	Check(a.program.instrs.size() == b.program.instrs.size()
		&& a.program.graphSeed == b.program.graphSeed, "pe.rt.same-program");
	Check(!ParseGraph("N 3 99\n", back), "pe.rt.garbage-rejected");
}

void TestTerrain() {
	// Swap the scaffold's Grid for a Terrain node and rewire.
	GraphDoc doc;
	BuildDefaultGraph(doc);
	uint32_t gridId = 0, layerId = 0;
	for (const auto& [id, n] : doc.nodes) {
		if (n.kind == NodeKind::Grid) gridId = id;
		if (n.kind == NodeKind::Layer) layerId = id;
	}
	Check(gridId != 0 && layerId != 0, "pe.terrain.scaffold-ids");
	doc.removeNode(gridId);
	const uint32_t terr = doc.addNode(NodeKind::Terrain, 20, 120);
	Check(doc.addLink(terr, "domain", layerId, "domain") != 0,
		"pe.terrain.rewire");
	CompileResult cr = CompileGraph(doc, 11);
	Check(cr.ok, "pe.terrain.compiles");
	if (!cr.ok) std::cout << "  err: " << cr.error << "\n";
	Check(cr.domainKind == DomainKind::Heightfield, "pe.terrain.kind");
	Check(cr.terrain.res == 64, "pe.terrain.default-res");
	Check(cr.terrain.size == 64.0f, "pe.terrain.default-size");
	// Field generation: dims, range, determinism.
	HeightfieldData a = BuildTerrainHeights(cr.terrain);
	HeightfieldData b = BuildTerrainHeights(cr.terrain);
	Check(a.valid(), "pe.terrain.valid");
	Check(a.width == 64 && a.height == 64, "pe.terrain.dims");
	bool inRange = true, varies = false;
	for (size_t k = 0; k < a.heights.size(); ++k) {
		if (a.heights[k] != b.heights[k]) { inRange = false; break; }
		if (a.heights[k] < 0.0f || a.heights[k] > cr.terrain.height) inRange = false;
		if (k > 0 && a.heights[k] != a.heights[0]) varies = true;
	}
	Check(inRange, "pe.terrain.range-deterministic");
	Check(varies, "pe.terrain.hills-vary");
	// Res clamp: tiny -> 8, huge -> 256.
	TerrainParams lo = cr.terrain;
	lo.res = 0;
	Check(BuildTerrainHeights(lo).width == 2, "pe.terrain.clamp-lo");
	TerrainParams hi = cr.terrain;
	hi.res = 100000;
	Check(BuildTerrainHeights(hi).width == 512, "pe.terrain.clamp-hi");
	// Domain sampling lands on the field.
	HeightfieldDomain dom;
	dom.field = &a;
	dom.extentMin = glm::vec2(0, 0);
	dom.extentMax = glm::vec2(64, 64);
	StableID id = MakeStableID(1, 0, TileID{}, 0, 0);
	DomainSample s = dom.sample(id);
	Check(std::abs(s.p.y - a.sampleHeight(s.p.x, s.p.z)) < 1e-5f,
		"pe.terrain.sample-on-field");
	// Round-trip preserves the Terrain node (kind 20 + seed int).
	const std::string text = SerializeGraph(doc);
	GraphDoc back;
	Check(ParseGraph(text, back), "pe.terrain.roundtrip-parses");
	const PNode* tn = back.find(terr);
	Check(tn && tn->kind == NodeKind::Terrain, "pe.terrain.roundtrip-kind");
	CompileResult cr2 = CompileGraph(back, 11);
	Check(cr2.ok && cr2.domainKind == DomainKind::Heightfield,
		"pe.terrain.roundtrip-compiles");
}

void TestUndo() {
	EditorDoc ed;
	const uint64_t gid = ed.addGraph("Undo");
	ed.pushUndo(); // baseline
	GraphDoc* g = ed.current();
	Check(g && g->nodes.size() == 7, "pe.undo.baseline");
	const uint32_t extra = g->addNode(NodeKind::Noise, 5, 5);
	ed.pushUndo();
	Check(ed.current()->nodes.size() == 8, "pe.undo.added");
	Check(ed.undo(), "pe.undo.steps-back");
	Check(ed.current()->nodes.size() == 7, "pe.undo.restored");
	Check(ed.current()->find(extra) == nullptr, "pe.undo.node-gone");
	Check(ed.redo(), "pe.undo.steps-forward");
	Check(ed.current()->nodes.size() == 8, "pe.undo.redone");
	// Dedup: pushing an unchanged snapshot is a no-op.
	const int pos = ed.undoPos;
	ed.pushUndo();
	Check(ed.undoPos == pos, "pe.undo.dedup");
	// Redo cleared by a new edit.
	Check(ed.undo(), "pe.undo.back-again");
	g = ed.current();
	g->addNode(NodeKind::Invert, 0, 0);
	ed.pushUndo();
	Check(!ed.redo(), "pe.undo.redo-cleared");
	// removeNode drops attached links; allocId reuses freed ids.
	GraphDoc* h = ed.current();
	size_t linksBefore = h->links.size();
	uint32_t victim = 0;
	for (const auto& [id, n] : h->nodes)
		if (n.kind == NodeKind::Density) victim = id;
	Check(victim != 0, "pe.undo.found-density");
	Check(h->removeNode(victim), "pe.undo.remove");
	Check(h->links.size() < linksBefore, "pe.undo.links-dropped");
	const uint32_t reused = h->addNode(NodeKind::Density, 0, 0);
	Check(reused == victim, "pe.undo.id-reuse");
	(void)gid;
}

int main() {
	TestScaffoldCompiles();
	TestAcceptScale();
	TestFieldChain();
	TestLinkRules();
	TestCycle();
	TestErrors();
	TestRoundTrip();
	TestTerrain();
	TestUndo();
	std::cout << (g_failures == 0 ? "ALL PASS " : "FAILURES ") << g_checks << " checks\n";
	return g_failures == 0 ? 0 : 1;
}
