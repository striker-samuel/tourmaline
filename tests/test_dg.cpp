// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// test_dg: the Maya-style DG core (editor_dg.h) + the placement graph
// ON it (schemas, plug semantics, dirty propagation, named-attribute
// serialization with legacy positional fallback). Headless, no ImGui.
//
// Hand-rolled runner (PlacementEditorTests style): PASS/FAIL lines,
// nonzero exit on any failure.

#include <cstdio>
#include <iostream>
#include <string>

#include "editor_dg.h"
#include "editor_placement.h"

namespace dg = editor::dg;
namespace placement = editor::placement;

int g_failures = 0;
int g_checks = 0;

void Check(bool cond, const char* name) {
	++g_checks;
	if (cond)
		std::cout << "PASS " << name << "\n";
	else {
		++g_failures;
		std::cout << "FAIL " << name << "\n";
	}
}

// ---- schema registry (the initialize() pattern) ------------------------------

static const dg::NodeType* TestTypes() {
	// A: authored input x -> output out; B: input b -> out; sink C.
	const dg::NodeType* a = dg::RegisterType({
		"test.a", "Test",
		{ dg::Num("x", "x", 1.0), dg::Pin("out", "o", dg::Channel::Field, true) },
		{} });
	// force the affects table to be built like MakeType does
	return a;
}

void TestRegistry() {
	const dg::NodeType* t1 = dg::RegisterType({
		"test.foo", "Test",
		{ dg::Num("input", "in", 3.0),
			dg::Str("label", "lbl", "hi"),
			dg::Int("count", "cnt", 7),
			dg::Pin("x", "x", dg::Channel::Field, false),
			dg::Pin("out", "o", dg::Channel::Field, true) },
		{} });
	Check(t1 != nullptr, "dg.register");
	// Idempotent registration returns the SAME definition set.
	const dg::NodeType* t2 = dg::RegisterType({ "test.foo", "Other", {}, {} });
	Check(t2 == t1, "dg.register-idempotent");
	Check(dg::FindType("test.foo") == t1, "dg.find");
	Check(dg::FindType("test.nope") == nullptr, "dg.find-miss");
	// Slots: storable only, per type, in declaration order (the
	// DataBlock contract — pins hold no values).
	const dg::AttributeDef* in = t1->findAttr("input");
	const dg::AttributeDef* lbl = t1->findAttr("label");
	const dg::AttributeDef* cnt = t1->findAttr("count");
	const dg::AttributeDef* pin = t1->findAttr("x");
	Check(in && in->slot == 0, "dg.slot-double0");
	Check(lbl && lbl->slot == 0, "dg.slot-string0");
	Check(cnt && cnt->slot == 0, "dg.slot-int0");
	Check(in && in->defDouble == 3.0, "dg.default-double");
	Check(cnt && cnt->defInt == 7, "dg.default-int");
	Check(lbl && std::string(lbl->defString) == "hi", "dg.default-string");
	Check(pin && pin->slot == 0 && !pin->is(dg::kAttrStorable),
		"dg.pin-no-slot");
	// Input/output split.
	Check(t1->findInput("x") && !t1->findInput("out"), "dg.find-input");
	Check(t1->findOutput("out") && !t1->findOutput("x"), "dg.find-output");
}

// ---- plug semantics (connections belong to plugs) ------------------------------

void TestConnections() {
	dg::NodeType src{};
	src.name = "test.src";
	src.attrs = { dg::Pin("out", "o", dg::Channel::Field, true) };
	dg::NodeType dst{};
	dst.name = "test.dst";
	dst.attrs = { dg::Pin("b", "b", dg::Channel::Field, false),
		dg::Pin("dom", "dom", dg::Channel::Domain, false) };
	const dg::NodeType* s = dg::RegisterType(std::move(src));
	const dg::NodeType* d = dg::RegisterType(std::move(dst));
	const dg::AttributeDef* sOut = s->findOutput("out");
	const dg::AttributeDef* dIn = d->findInput("b");
	const dg::AttributeDef* dDom = d->findInput("dom");
	// Valid: output -> input, channel match.
	Check(dg::CanConnect({ 1, sOut }, { 2, dIn }), "dg.connect-ok");
	// Channel mismatch (Field -> Domain).
	Check(!dg::CanConnect({ 1, sOut }, { 2, dDom }),
		"dg.connect-channel-mismatch");
	// Same instance never.
	Check(!dg::CanConnect({ 1, sOut }, { 1, dIn }), "dg.connect-same-node");
	// Reverse direction (input as source) rejected.
	Check(!dg::CanConnect({ 2, dIn }, { 1, sOut }), "dg.connect-reversed");
	// Non-plug attrs (authored values) rejected: a Num has no
	// connectable flag.
	const dg::NodeType* n = dg::RegisterType({ "test.n", "T",
		{ dg::Num("v", "v", 0.0) }, {} });
	Check(!dg::CanConnect({ 1, n->findAttr("v") }, { 2, dIn }),
		"dg.connect-value-attr");
}

// ---- dirty propagation (attributeAffects is metadata, not dataflow) ----------

static const dg::NodeType* TestDirtyTypes() {
	// gen: authored x -> out; mid: in b -> out; sink: in only.
	dg::NodeType gen{};
	gen.name = "test.gen";
	gen.attrs = { dg::Num("x", "x", 0.0),
		dg::Pin("out", "o", dg::Channel::Field, true) };
	gen.affects = { { "x", "out" } };
	dg::NodeType mid{};
	mid.name = "test.mid";
	mid.attrs = { dg::Pin("b", "b", dg::Channel::Field, false),
		dg::Pin("out", "o", dg::Channel::Field, true) };
	mid.affects = { { "b", "out" } };
	dg::NodeType sink{};
	sink.name = "test.sink";
	sink.attrs = { dg::Pin("in", "in", dg::Channel::Field, false) };
	dg::NodeType isle{}; // disconnected island with the same shape as gen
	isle.name = "test.isle";
	isle.attrs = gen.attrs;
	isle.affects = { { "x", "out" } };
	const dg::NodeType* g = dg::RegisterType(std::move(gen));
	dg::RegisterType(std::move(mid));
	dg::RegisterType(std::move(sink));
	dg::RegisterType(std::move(isle));
	return g;
}

void TestDirty() {
	const dg::NodeType* g = TestDirtyTypes();
	const dg::NodeType* mid = dg::FindType("test.mid");
	const dg::NodeType* sink = dg::FindType("test.sink");
	const dg::NodeType* isle = dg::FindType("test.isle");
	// gen(1).out -> mid(2).b ; mid(2).out -> sink(3).in
	dg::Connection c1{ dg::Plug{ 1, g->findOutput("out") },
		dg::Plug{ 2, mid->findInput("b") } };
	dg::Connection c2{ dg::Plug{ 2, mid->findOutput("out") },
		dg::Plug{ 3, sink->findInput("in") } };
	std::vector<dg::Connection> conns{ c1, c2 };
	auto typeOf = [&](uint32_t node) -> const dg::NodeType* {
		switch (node) {
		case 1: return g;
		case 2: return mid;
		case 3: return sink;
		case 4: return isle;
		}
		return nullptr;
	};
	dg::DirtySet dirty;
	dirty.mark(1, g->findAttr("x"), typeOf, conns);
	// attributeAffects: x dirties out (same instance).
	Check(dirty.dirty(1, g->findAttr("out")), "dg.dirty.affects-out");
	// Connection carries it downstream: mid.b -> mid.out -> sink.in.
	Check(dirty.dirty(2, mid->findAttr("b")), "dg.dirty.conn-dest");
	Check(dirty.dirty(2, mid->findAttr("out")), "dg.dirty.conn-affects");
	Check(dirty.dirty(3, sink->findAttr("in")), "dg.dirty.downstream");
	// Never upstream and never unconnected instances.
	Check(!dirty.dirty(4, isle->findAttr("out")), "dg.dirty.island-clean");
	Check(!dirty.dirty(1, g->findAttr("x")) == false,
		"dg.dirty.seed-itself");
	Check(dirty.dirtyNode(3), "dg.dirty.node");
	dirty.clear();
	Check(dirty.size() == 0, "dg.dirty.clear");
}

// ---- placement schemas (runtime on the DG) -------------------------------------

void TestPlacementSchemas() {
	const dg::NodeType* dens = placement::PlacementType(placement::NodeKind::Density);
	Check(dens != nullptr, "pl.type-registered");
	Check(dens->name == "placement.density", "pl.type-name");
	Check(dens->findAttr("value") && dens->findAttr("value")->slot == 0,
		"pl.slot-value");
	// Pins derive from the schema (single source of truth).
	std::vector<placement::PinDef> layerIn = placement::NodeInputs(
		placement::NodeKind::Layer);
	Check(layerIn.size() == 5, "pl.layer-pins");
	if (layerIn.size() == 5) {
		Check(std::string(layerIn[0].name) == "domain"
			&& layerIn[2].name == std::string("protos"),
			"pl.layer-pin-order");
	}
	// Kind <-> type name.
	auto kind = placement::KindOfTypeName("placement.terrain");
	Check(kind && *kind == placement::NodeKind::Terrain, "pl.kind-of-name");
	Check(!placement::KindOfTypeName("nope.x").has_value(),
		"pl.kind-of-name-miss");
	// Defaults through the schema (DataBlock defaults).
	placement::PNode n;
	n.kind = placement::NodeKind::Noise;
	placement::PNode viaAdd;
	placement::GraphDoc doc;
	const uint32_t id = doc.addNode(placement::NodeKind::Noise, 0, 0);
	const placement::PNode* added = doc.find(id);
	Check(added && added->f.size() == 3 && std::abs(added->f[0] - 0.02f)
		< 1e-6f && added->f[1] == 0.0f && added->f[2] == 0.0f,
		"pl.defaults-noise");
	// Named accessors (the MDataHandle side).
	placement::SetAttr(*doc.find(id), "frequency", 0.07);
	Check(std::abs(placement::GetAttr(*doc.find(id), "frequency") - 0.07)
		< 1e-6, "pl.set-get-double");
}

// ---- tour format (named attributes + legacy fallback) --------------------------

void TestSerialization() {
	// New named form round-trips.
	placement::GraphDoc doc;
	const uint32_t grid = doc.addNode(placement::NodeKind::Grid, 0, 0);
	const uint32_t layer = doc.addNode(placement::NodeKind::Layer, 1, 1);
	doc.addLink(grid, "domain", layer, "domain");
	const std::string text = placement::SerializeGraph(doc);
	placement::GraphDoc back;
	Check(placement::ParseGraph(text, back), "pl.serialize-parses");
	Check(back.nodes.size() == doc.nodes.size(), "pl.serialize-nodes");
	Check(back.links.size() == 1, "pl.serialize-links");
	const placement::PNode* g = back.find(grid);
	Check(g && g->kind == placement::NodeKind::Grid && g->f.size() == 7
		&& std::abs(g->f[3] - 64.0f) < 1e-5f, "pl.serialize-grid-values");
	const placement::PNode* l = back.find(layer);
	Check(l && l->kind == placement::NodeKind::Layer && l->f.size() == 1
		&& l->f[0] == 4096.0f && l->s.size() == 1
		&& l->s[0] == "1234", "pl.serialize-layer-seed");
	// The node line SPEAKS named attributes now.
	const std::string line = placement::SerializeNode(*l);
	Check(line.rfind("n placement.layer", 0) == 0
		&& line.find("candidates=4096") != std::string::npos
		&& line.find("seed=1234") != std::string::npos,
		"pl.line-named-attrs");
	// Legacy positional line still parses (slot-compat).
	placement::PNode legacy;
	Check(placement::ParseNode("0 7 0 0 0 64 64 8 8 0", legacy)
		&& legacy.kind == placement::NodeKind::Grid
		&& legacy.f.size() == 7, "pl.legacy-positional");
	// Legacy string section.
	placement::PNode legacyS;
	Check(placement::ParseNode("19 1 4096 0 | 1234", legacyS)
		&& legacyS.kind == placement::NodeKind::Layer
		&& legacyS.s.size() == 1 && legacyS.s[0] == "1234",
		"pl.legacy-strings");
	// Unknown attributes in a named line skip (forward compat).
	placement::PNode fwd;
	Check(placement::ParseNode("n placement.density value=0.9 future=1",
		fwd) && std::abs(fwd.f[0] - 0.9f) < 1e-6, "pl.named-forward-compat");
	// Unknown type name rejects.
	placement::PNode bad;
	Check(!placement::ParseNode("n nope.nope v=1", bad),
		"pl.named-unknown-type");
}

int main() {
	TestRegistry();
	TestConnections();
	TestDirty();
	TestPlacementSchemas();
	TestSerialization();
	std::cout << g_checks - g_failures << "/" << g_checks << " passed\n";
	return g_failures == 0 ? 0 : 1;
}
