// editor_placement_dg.cpp — the placement graph ON the Maya-style DG
// core (editor_dg.h). Every NodeKind registers a TYPE here (the
// initialize()/addAttribute() pattern): pin attributes (connectable,
// channel-typed) and parameter attributes (keyable, storable, with
// defaults) in EXACT storage-slot order — the PNode f/i/s vectors ARE
// the per-instance DataBlock, so the registry is the single source of
// truth for pins, defaults, names AND the tour's named-attribute lines.
//
// attributeAffects: every authored value dirties the type's output plug
// — the metadata the DirtySet walk uses (params -> out -> connections
// -> downstream inputs).

#include "editor_placement.h"

#include "editor_dg.h"

#include <unordered_map>

namespace editor::placement {
namespace {

using namespace smvk::placement;
namespace dg = editor::dg;

dg::NodeType MakeType(std::string name, const char* category,
	std::vector<dg::AttributeDef> attrs) {
	dg::NodeType t;
	t.name = std::move(name);
	t.category = category;
	t.attrs = std::move(attrs);
	// attributeAffects: every authored attr -> every output pin.
	std::vector<std::string> outputs;
	for (const dg::AttributeDef& a : t.attrs)
		if (a.is(dg::kAttrOutput))
			outputs.push_back(a.longName);
	for (const dg::AttributeDef& a : t.attrs) {
		if (a.is(dg::kAttrOutput))
			continue;
		for (const std::string& o : outputs)
			t.affects.push_back({ a.longName, o });
	}
	return t;
}

const dg::NodeType* Register(dg::NodeType t) {
	return dg::RegisterType(std::move(t));
}

struct KindType {
	NodeKind kind;
	const dg::NodeType* type;
};

// The full schema table. Attribute order = slot order = the historical
// positional f/i/s layout (slot-compatible: legacy files and the
// compiler keep working; the DG only adds NAMES + semantics on top).
std::vector<KindType> BuildAll() {
	std::vector<KindType> all;
	auto add = [&](NodeKind k, dg::NodeType t) {
		all.push_back({ k, Register(std::move(t)) });
	};
	using dg::Num;
	using dg::Int;
	using dg::Str;
	using dg::Pin;
	using C = dg::Channel;

	add(NodeKind::Grid, MakeType("placement.grid", "Domain", {
		Num("originX", "ox", 0.0), Num("originY", "oy", 0.0),
		Num("originZ", "oz", 0.0), Num("extentX", "ex", 64.0),
		Num("extentZ", "ez", 64.0), Num("cellsX", "cx", 8.0),
		Num("cellsZ", "cz", 8.0),
		Pin("domain", "dom", C::Domain, true) }));

	add(NodeKind::Mesh, MakeType("placement.mesh", "Domain", {
		Int("worldNodeId", "wnode", -1),
		Pin("domain", "dom", C::Domain, true) }));

	add(NodeKind::Density, MakeType("placement.density", "Field", {
		Num("value", "v", 0.5),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Noise, MakeType("placement.noise", "Field", {
		Num("frequency", "freq", 0.02), Num("offsetX", "ox", 0.0),
		Num("offsetZ", "oz", 0.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Slope, MakeType("placement.slope", "Field", {
		Num("flatDegrees", "flat", 0.0), Num("steepDegrees", "steep", 40.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Altitude, MakeType("placement.altitude", "Field", {
		Num("low0", "lo0", 0.0), Num("low1", "lo1", 5.0),
		Num("high0", "hi0", 15.0), Num("high1", "hi1", 25.0),
		Pin("out", "o", C::Field, true) }));

	for (NodeKind k : { NodeKind::Add, NodeKind::Mul, NodeKind::Min,
			NodeKind::Max })
		add(k, MakeType(
			std::string("placement.")
				+ (k == NodeKind::Add ? "add"
					: k == NodeKind::Mul ? "mul"
						: k == NodeKind::Min ? "min" : "max"),
			"Field", {
			Pin("a", "a", C::Field, false), Pin("b", "b", C::Field, false),
			Pin("out", "o", C::Field, true) }));

	add(NodeKind::Clamp, MakeType("placement.clamp01", "Field", {
		Pin("x", "x", C::Field, false),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Remap, MakeType("placement.remap", "Field", {
		Pin("x", "x", C::Field, false),
		Num("in0", "in0", 0.0), Num("in1", "in1", 1.0),
		Num("out0", "out0", 0.0), Num("out1", "out1", 1.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Smoothstep, MakeType("placement.smoothstep", "Field", {
		Pin("x", "x", C::Field, false),
		Num("edge0", "e0", 0.0), Num("edge1", "e1", 1.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Power, MakeType("placement.power", "Field", {
		Pin("x", "x", C::Field, false),
		Num("exponent", "exp", 2.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Invert, MakeType("placement.invert", "Field", {
		Pin("x", "x", C::Field, false),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Accept, MakeType("placement.accept", "Flow", {
		Pin("density", "dens", C::Field, false),
		Num("scale", "scale", 1.0),
		Pin("out", "o", C::Field, true) }));

	add(NodeKind::Prototypes, MakeType("placement.prototypes", "Flow", {
		Num("slopeRock", "slope", 0.0), Num("threshold", "thr", 35.0),
		Num("weight0", "w0", 1.0), Num("radius0", "r0", 0.6),
		Num("weight1", "w1", 0.25), Num("radius1", "r1", 1.2),
		Str("uri0", "u0", "Oak"), Str("uri1", "u1", "Rock"),
		Pin("protos", "protos", C::Protos, true) }));

	add(NodeKind::Transform, MakeType("placement.transform", "Flow", {
		Num("minScale", "minS", 0.8), Num("maxScale", "maxS", 1.2),
		Pin("xform", "xform", C::Xform, true) }));

	add(NodeKind::Spacing, MakeType("placement.spacing", "Flow", {
		Num("minSpacing", "minD", 0.0),
		Pin("spacing", "spacing", C::Spacing, true) }));

	add(NodeKind::Layer, MakeType("placement.layer", "Output", {
		Pin("domain", "dom", C::Domain, false),
		Pin("density", "dens", C::Field, false),
		Pin("protos", "protos", C::Protos, false),
		Pin("xform", "xform", C::Xform, false),
		Pin("spacing", "spacing", C::Spacing, false),
		Num("candidates", "cand", 4096.0),
		Str("seed", "seed", "1234") }));

	add(NodeKind::Terrain, MakeType("placement.terrain", "Domain", {
		Num("originX", "ox", 0.0), Num("originZ", "oz", 0.0),
		Num("size", "size", 64.0), Num("resolution", "res", 64.0),
		Num("height", "height", 8.0), Num("frequency", "freq", 0.02),
		Int("seed32", "seed", 1), Int("terrainBind", "bind", -1),
		Pin("domain", "dom", C::Domain, true) }));

	return all;
}

} // namespace

const dg::NodeType* PlacementType(NodeKind kind) {
	static const std::vector<KindType> kAll = BuildAll();
	static const std::unordered_map<int, const dg::NodeType*> kByKind = []() {
		std::unordered_map<int, const dg::NodeType*> m;
		for (const KindType& kt : kAll)
			m[int(kt.kind)] = kt.type;
		return m;
	}();
	auto it = kByKind.find(int(kind));
	return it != kByKind.end() ? it->second : nullptr;
}

std::optional<NodeKind> KindOfTypeName(const std::string& typeName) {
	// Same registration table as PlacementType (ordering guaranteed).
	static const std::unordered_map<std::string, int> kByName = []() {
		std::unordered_map<std::string, int> m;
		for (const KindType& kt : BuildAll())
			m[kt.type->name] = int(kt.kind);
		return m;
	}();
	auto it = kByName.find(typeName);
	if (it == kByName.end())
		return std::nullopt;
	return NodeKind(it->second);
}

// ---- DataBlock access (the MDataHandle side over PNode's storage) ----------

double GetAttr(const PNode& n, const char* name) {
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	return (a && a->slot < n.f.size()) ? double(n.f[a->slot]) : 0.0;
}
void SetAttr(PNode& n, const char* name, double v) {
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	if (a && a->type == dg::AttrType::Double) {
		if (n.f.size() <= a->slot)
			n.f.resize(a->slot + 1, 0.0f);
		n.f[a->slot] = float(v);
	}
}
int64_t GetAttrI(const PNode& n, const char* name) {
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	return (a && a->slot < n.i.size()) ? int64_t(n.i[a->slot]) : 0;
}
void SetAttrI(PNode& n, const char* name, int64_t v) {
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	if (a && a->type == dg::AttrType::Int) {
		if (n.i.size() <= a->slot)
			n.i.resize(a->slot + 1, 0);
		n.i[a->slot] = int(v);
	}
}
const std::string& GetAttrS(const PNode& n, const char* name) {
	static const std::string kEmpty;
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	return (a && a->slot < n.s.size()) ? n.s[a->slot] : kEmpty;
}
void SetAttrS(PNode& n, const char* name, const std::string& v) {
	const dg::NodeType* t = PlacementType(n.kind);
	const dg::AttributeDef* a = t ? t->findAttr(name) : nullptr;
	if (a && a->type == dg::AttrType::String) {
		if (n.s.size() <= a->slot)
			n.s.resize(a->slot + 1);
		n.s[a->slot] = v;
	}
}

} // namespace editor::placement
