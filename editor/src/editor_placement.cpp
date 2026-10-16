// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// PlacementGraph editor: document model, compiler, persistence, undo.
// EditorApp integration (preview/mesh-extract/save-load) lives in
// editor_placement_app.cpp; canvas/panels/overlay in
// editor_placement_ui.cpp. This TU is engine-only (unit-testable).

#include "editor_placement.h"

#include "editor_dg.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace editor::placement {
namespace {

using namespace smvk::placement;
using namespace detail;
namespace dg = editor::dg;

} // namespace

// ---- names / pins -----------------------------------------------------------
// The DG registry (editor_placement_dg.cpp) is the single source of
// truth: names, categories, pins and defaults all read from the
// registered TYPE schemas (the initialize()/addAttribute() layer).
const char* NodeKindName(NodeKind k) {
	const dg::NodeType* t = PlacementType(k);
	return t ? t->name.c_str() : "?";
}
const char* NodeKindCategory(NodeKind k) {
	const dg::NodeType* t = PlacementType(k);
	return t ? t->category.c_str() : "?";
}
uint32_t NodeKindCapabilities(NodeKind) {
	return PlacementCapability_CPU | PlacementCapability_GPU;
}
const char* PinTypeName(PinType t) {
	switch (t) {
	case PinType::Domain: return "domain";
	case PinType::Field: return "field";
	case PinType::Protos: return "protos";
	case PinType::Xform: return "xform";
	case PinType::Spacing: return "spacing";
	default: return "?";
	}
}
namespace {
dg::Channel PinChannel(PinType t) {
	switch (t) {
	case PinType::Domain: return dg::Channel::Domain;
	case PinType::Field: return dg::Channel::Field;
	case PinType::Protos: return dg::Channel::Protos;
	case PinType::Xform: return dg::Channel::Xform;
	case PinType::Spacing: return dg::Channel::Spacing;
	}
	return dg::Channel::None;
}
PinType ChannelPin(dg::Channel c) {
	switch (c) {
	case dg::Channel::Domain: return PinType::Domain;
	case dg::Channel::Field: return PinType::Field;
	case dg::Channel::Protos: return PinType::Protos;
	case dg::Channel::Xform: return PinType::Xform;
	case dg::Channel::Spacing: return PinType::Spacing;
	}
	return PinType::Field;
}
} // namespace
std::vector<PinDef> NodeInputs(NodeKind k) {
	std::vector<PinDef> pins;
	if (const dg::NodeType* t = PlacementType(k))
		for (const dg::AttributeDef& a : t->attrs)
			if (a.is(dg::kAttrConnectable) && !a.is(dg::kAttrOutput))
				pins.push_back({ a.longName, ChannelPin(a.channel) });
	return pins;
}
std::vector<PinDef> NodeOutputs(NodeKind k) {
	std::vector<PinDef> pins;
	if (const dg::NodeType* t = PlacementType(k))
		for (const dg::AttributeDef& a : t->attrs)
			if (a.is(dg::kAttrConnectable) && a.is(dg::kAttrOutput))
				pins.push_back({ a.longName, ChannelPin(a.channel) });
	return pins;
}

// ---- GraphDoc ------------------------------------------------------------------
PNode* GraphDoc::find(uint32_t id) {
	auto it = nodes.find(id);
	return it != nodes.end() ? &it->second : nullptr;
}
const PNode* GraphDoc::find(uint32_t id) const {
	auto it = nodes.find(id);
	return it != nodes.end() ? &it->second : nullptr;
}
const PLink* GraphDoc::inputLink(uint32_t node, const std::string& pin) const {
	for (const auto& [id, l] : links)
		if (l.to == node && l.toPin == pin) return &l;
	return nullptr;
}
uint32_t GraphDoc::allocId() {
	// Reuse the lowest free id across nodes AND links (keeps files
	// compact and ids small after deletions).
	uint32_t id = 1;
	while (nodes.contains(id) || links.contains(id)) ++id;
	if (id >= nextId) nextId = id + 1;
	return id;
}
namespace {
// Defaults come from the TYPE schema (the DataBlock's declared default
// values) — the historical positional f/i/s layout, by slot.
void DefaultParams(PNode& n) {
	const dg::NodeType* t = PlacementType(n.kind);
	if (!t)
		return;
	uint32_t nd = 0, ni = 0, ns = 0;
	for (const dg::AttributeDef& a : t->attrs) {
		if (!a.is(dg::kAttrStorable))
			continue; // pins hold no values
		switch (a.type) {
		case dg::AttrType::Double:
			if (n.f.size() <= a.slot)
				n.f.resize(a.slot + 1, 0.0f);
			n.f[a.slot] = float(a.defDouble);
			nd = std::max(nd, a.slot + 1);
			break;
		case dg::AttrType::Int:
			if (n.i.size() <= a.slot)
				n.i.resize(a.slot + 1, 0);
			n.i[a.slot] = int(a.defInt);
			ni = std::max(ni, a.slot + 1);
			break;
		case dg::AttrType::String:
			if (n.s.size() <= a.slot)
				n.s.resize(a.slot + 1);
			n.s[a.slot] = a.defString;
			ns = std::max(ns, a.slot + 1);
			break;
		}
	}
	n.f.resize(nd);
	n.i.resize(ni);
	n.s.resize(ns);
}
} // namespace
uint32_t GraphDoc::addNode(NodeKind kind, float x, float y) {
	PNode n;
	n.id = allocId();
	n.kind = kind;
	n.pos[0] = x; n.pos[1] = y;
	DefaultParams(n);
	nodes[n.id] = n;
	return n.id;
}
bool GraphDoc::removeNode(uint32_t id) {
	if (!nodes.contains(id)) return false;
	std::vector<uint32_t> dead;
	for (const auto& [lid, l] : links)
		if (l.from == id || l.to == id) dead.push_back(lid);
	for (uint32_t lid : dead) links.erase(lid);
	nodes.erase(id);
	return true;
}
uint32_t GraphDoc::addLink(uint32_t from, const std::string& fromPin,
	uint32_t to, const std::string& toPin) {
	const PNode* a = find(from);
	const PNode* b = find(to);
	if (!a || !b || from == to) return 0;
	// Connection semantics live on PLUGS (the Maya contract): resolve
	// both endpoints through the type schemas, then validate readable/
	// writable/connectable + channel match in one place.
	const dg::NodeType* ta = PlacementType(a->kind);
	const dg::NodeType* tb = PlacementType(b->kind);
	const dg::AttributeDef* outAttr =
		ta ? ta->findOutput(fromPin.c_str()) : nullptr;
	const dg::AttributeDef* inAttr =
		tb ? tb->findInput(toPin.c_str()) : nullptr;
	if (!outAttr || !inAttr)
		return 0;
	if (!dg::CanConnect(dg::Plug{ from, outAttr }, dg::Plug{ to, inAttr }))
		return 0;
	if (inputLink(to, toPin)) return 0; // inputs take one wire
	PLink l;
	l.id = allocId();
	l.from = from; l.fromPin = fromPin; l.to = to; l.toPin = toPin;
	links[l.id] = l;
	return l.id;
}
bool GraphDoc::removeLink(uint32_t id) {
	return links.erase(id) > 0;
}
void BuildDefaultGraph(GraphDoc& doc) {
	doc.nodes.clear();
	doc.links.clear();
	doc.nextId = 1;
	const uint32_t grid = doc.addNode(NodeKind::Grid, 20, 120);
	const uint32_t dens = doc.addNode(NodeKind::Density, 250, 60);
	const uint32_t acc = doc.addNode(NodeKind::Accept, 480, 120);
	const uint32_t layer = doc.addNode(NodeKind::Layer, 710, 120);
	const uint32_t protos = doc.addNode(NodeKind::Prototypes, 480, 280);
	const uint32_t xform = doc.addNode(NodeKind::Transform, 480, 430);
	const uint32_t spacing = doc.addNode(NodeKind::Spacing, 480, 550);
	doc.addLink(grid, "domain", layer, "domain");
	doc.addLink(dens, "out", acc, "density");
	doc.addLink(acc, "out", layer, "density");
	doc.addLink(protos, "protos", layer, "protos");
	doc.addLink(xform, "xform", layer, "xform");
	doc.addLink(spacing, "spacing", layer, "spacing");
}

// ---- serialization ---------------------------------------------------------------
// Node lines speak the DG: `n <type> <attr>=<value>... | <strAttr>=<v>...`
// (attributes BY NAME, from the type schema — Maya setAttr flavor).
// Legacy positional lines (`<kindInt> <nf> <f...> <ni> <i...> | <s...>`)
// parse unchanged (slot-compatible schema keeps old files loadable).
std::string SerializeNode(const PNode& n) {
	const dg::NodeType* t = PlacementType(n.kind);
	if (!t)
		return "0 0 0"; // unknown kind: minimal legacy line
	std::string o = "n " + t->name;
	for (const dg::AttributeDef& a : t->attrs) {
		if (!a.is(dg::kAttrStorable) || a.type == dg::AttrType::String)
			continue;
		if (a.type == dg::AttrType::Double) {
			const float v = (a.slot < n.f.size()) ? n.f[a.slot] : 0.0f;
			o += " " + std::string(a.longName) + "=" + FStr(v);
		}
		else {
			const int v = (a.slot < n.i.size()) ? n.i[a.slot] : 0;
			o += " " + std::string(a.longName) + "=" + std::to_string(v);
		}
	}
	for (const dg::AttributeDef& a : t->attrs) {
		if (!a.is(dg::kAttrStorable) || a.type != dg::AttrType::String)
			continue;
		const std::string& v =
			(a.slot < n.s.size()) ? n.s[a.slot] : std::string();
		o += " | " + std::string(a.longName) + "=" + v;
	}
	return o;
}
bool ParseNode(const std::string& val, PNode& out) {
	out.f.clear(); out.i.clear(); out.s.clear();
	// Split off |-separated pieces first (strings section).
	std::vector<std::string> parts;
	{
		size_t start = 0;
		for (;;) {
			size_t bar = val.find('|', start);
			std::string piece = val.substr(start,
				bar == std::string::npos ? std::string::npos : bar - start);
			// trim one surrounding space
			while (!piece.empty() && piece.front() == ' ') piece.erase(piece.begin());
			while (!piece.empty() && piece.back() == ' ') piece.pop_back();
			parts.push_back(piece);
			if (bar == std::string::npos) break;
			start = bar + 1;
		}
	}
	// ---- named form: "n <type> <attr>=<v>..." ----
	if (!parts[0].empty() && parts[0].rfind("n ", 0) == 0) {
		std::istringstream head(parts[0]);
		std::string marker, typeName;
		if (!(head >> marker >> typeName))
			return false;
		const auto kind = KindOfTypeName(typeName);
		if (!kind)
			return false;
		out.kind = *kind;
		// Schema defaults first (same fill as DefaultParams), then the
		// named overrides below.
		{
			const dg::NodeType* t = PlacementType(out.kind);
			uint32_t nd = 0, ni = 0, ns = 0;
			for (const dg::AttributeDef& a : t->attrs) {
				if (!a.is(dg::kAttrStorable))
					continue;
				switch (a.type) {
				case dg::AttrType::Double:
					if (out.f.size() <= a.slot)
						out.f.resize(a.slot + 1, 0.0f);
					out.f[a.slot] = float(a.defDouble);
					nd = std::max(nd, a.slot + 1);
					break;
				case dg::AttrType::Int:
					if (out.i.size() <= a.slot)
						out.i.resize(a.slot + 1, 0);
					out.i[a.slot] = int(a.defInt);
					ni = std::max(ni, a.slot + 1);
					break;
				case dg::AttrType::String:
					if (out.s.size() <= a.slot)
						out.s.resize(a.slot + 1);
					out.s[a.slot] = a.defString;
					ns = std::max(ns, a.slot + 1);
					break;
				}
			}
			out.f.resize(nd);
			out.i.resize(ni);
			out.s.resize(ns);
		}
		// Numeric/typed overrides: "<attr>=<value>" tokens.
		auto applyAttr = [&](const std::string& tok) {
			const size_t eq = tok.find('=');
			if (eq == std::string::npos)
				return;
			const std::string name = tok.substr(0, eq);
			const std::string value = tok.substr(eq + 1);
			const dg::NodeType* t = PlacementType(out.kind);
			const dg::AttributeDef* a = t ? t->findAttr(name.c_str())
				: nullptr;
			if (!a || !a->is(dg::kAttrStorable))
				return; // unknown attr: forward-compat skip
			switch (a->type) {
			case dg::AttrType::Double:
				SetAttr(out, name.c_str(), StrF(value, 0.0));
				break;
			case dg::AttrType::Int:
				SetAttrI(out, name.c_str(), StrI(value, 0));
				break;
			case dg::AttrType::String:
				SetAttrS(out, name.c_str(), value);
				break;
			}
		};
		{
			std::istringstream nums(parts[0]);
			std::string tok;
			nums >> tok >> tok; // "n" + type name
			while (nums >> tok)
				applyAttr(tok);
		}
		// String overrides (| pieces after the first): "<attr>=<value>".
		for (size_t k = 1; k < parts.size(); ++k)
			if (!parts[k].empty())
				applyAttr(parts[k]);
		return true;
	}
	// ---- legacy positional form ----
	std::istringstream head(parts[0]);
	int kind = -1, nf = -1, ni = -1;
	if (!(head >> kind >> nf)) return false;
	if (kind < 0 || kind >= (int)NodeKind::Count || nf < 0 || nf > 64) return false;
	out.kind = (NodeKind)kind;
	for (int k = 0; k < nf; ++k) {
		float v = 0;
		if (!(head >> v)) return false;
		out.f.push_back(v);
	}
	if (!(head >> ni)) return false;
	if (ni < 0 || ni > 8) return false;
	for (int k = 0; k < ni; ++k) {
		int v = 0;
		if (!(head >> v)) return false;
		out.i.push_back(v);
	}
	for (size_t k = 1; k < parts.size(); ++k) out.s.push_back(parts[k]);
	return true;
}
std::string SerializeLink(const PLink& l) {
	return std::to_string(l.from) + " " + l.fromPin + " " +
		std::to_string(l.to) + " " + l.toPin;
}
bool ParseLink(const std::string& val, PLink& out) {
	std::istringstream in(val);
	return (in >> out.from >> out.fromPin >> out.to >> out.toPin) ? true : false;
}
std::string SerializeGraph(const GraphDoc& doc) {
	std::string o = "N " + doc.name + "\n";
	for (const auto& [id, n] : doc.nodes) {
		o += "N " + std::to_string(id) + " " + SerializeNode(n) + "\n";
		o += "P " + std::to_string(id) + " " + FStr(n.pos[0]) + " " + FStr(n.pos[1]) + "\n";
	}
	for (const auto& [id, l] : doc.links)
		o += "L " + std::to_string(id) + " " + SerializeLink(l) + "\n";
	return o;
}
bool ParseGraph(const std::string& text, GraphDoc& out) {
	GraphDoc doc;
	std::istringstream in(text);
	std::string line;
	while (std::getline(in, line)) {
		if (line.size() < 2 || line[1] != ' ') continue;
		const char tag = line[0];
		const std::string rest = line.substr(2);
		if (tag == 'N' && !rest.empty() && rest[0] != ' ' && isdigit((unsigned char)rest[0])) {
			// Node line: "N <id> <nodeval>"
			std::istringstream h(rest);
			uint32_t nid = 0;
			if (!(h >> nid)) return false;
			std::string nval;
			std::getline(h, nval);
			while (!nval.empty() && nval.front() == ' ') nval.erase(nval.begin());
			PNode n;
			if (!ParseNode(nval, n)) return false;
			n.id = nid;
			doc.nodes[nid] = n;
		}
		else if (tag == 'N') { doc.name = rest; }
		else if (tag == 'P') {
			std::istringstream h(rest);
			uint32_t nid = 0;
			float x = 0, y = 0;
			if (!(h >> nid >> x >> y)) continue; // positions are cosmetic
			auto it = doc.nodes.find(nid);
			if (it != doc.nodes.end()) { it->second.pos[0] = x; it->second.pos[1] = y; }
		}
		else if (tag == 'L') {
			std::istringstream h(rest);
			uint32_t lid = 0;
			if (!(h >> lid)) return false;
			std::string lval;
			std::getline(h, lval);
			while (!lval.empty() && lval.front() == ' ') lval.erase(lval.begin());
			PLink l;
			if (!ParseLink(lval, l)) return false;
			l.id = lid;
			doc.links[lid] = l;
		}
	}
	// Rebuild nextId past all used ids.
	uint32_t mx = 0;
	for (const auto& [id, n] : doc.nodes) mx = std::max(mx, id);
	for (const auto& [id, l] : doc.links) mx = std::max(mx, id);
	doc.nextId = mx + 1;
	out = doc;
	return true;
}

// ---- compiler ----------------------------------------------------------------------
namespace {
struct Ctx {
	const GraphDoc* doc = nullptr;
	CompileResult* out = nullptr;
	std::unordered_map<uint32_t, int> memo; // node id -> value index
	std::unordered_map<uint32_t, bool> busy; // cycle detection
	bool emitField(uint32_t nid, int& idx) {
		auto m = memo.find(nid);
		if (m != memo.end()) { idx = m->second; return true; }
		if (busy[nid]) {
			out->error = std::string("cycle at '") + NodeKindName(doc->find(nid)->kind) + "'";
			return false;
		}
		const PNode* n = doc->find(nid);
		if (!n) { out->error = "dangling link"; return false; }
		busy[nid] = true;
		const auto& F = n->f;
		auto need = [&](const std::string& pin, int& sub) {
			const PLink* l = doc->inputLink(nid, pin);
			if (!l) {
				out->error = std::string(NodeKindName(n->kind)) + ": missing '" + pin + "'";
				return false;
			}
			return emitField(l->from, sub);
		};
		FieldInstr ins{};
		auto push = [&]() {
			idx = (int)out->program.instrs.size();
			out->program.instrs.push_back(ins);
			out->instrToNode.push_back((int)nid);
			memo[nid] = idx;
			busy[nid] = false;
			return true;
		};
		int a = 0, b = 0;
		switch (n->kind) {
		case NodeKind::Density:
			ins.op = FieldOp::Const; ins.params[0] = F.size() > 0 ? F[0] : 0.5f; return push();
		case NodeKind::Noise:
			ins.op = FieldOp::Noise2;
			ins.params[0] = F.size() > 0 ? F[0] : 0.02f;
			ins.params[1] = F.size() > 1 ? F[1] : 0;
			ins.params[2] = F.size() > 2 ? F[2] : 0;
			return push();
		case NodeKind::Slope:
			ins.op = FieldOp::SlopeMask;
			ins.params[0] = F.size() > 0 ? F[0] : 0;
			ins.params[1] = F.size() > 1 ? F[1] : 40;
			return push();
		case NodeKind::Altitude:
			ins.op = FieldOp::AltitudeMask;
			ins.params[0] = F.size() > 0 ? F[0] : 0;
			ins.params[1] = F.size() > 1 ? F[1] : 5;
			ins.params[2] = F.size() > 2 ? F[2] : 15;
			ins.params[3] = F.size() > 3 ? F[3] : 25;
			return push();
		case NodeKind::Add: ins.op = FieldOp::Add; break;
		case NodeKind::Mul: ins.op = FieldOp::Mul; break;
		case NodeKind::Min: ins.op = FieldOp::Min; break;
		case NodeKind::Max: ins.op = FieldOp::Max; break;
		case NodeKind::Clamp: ins.op = FieldOp::Clamp01; break;
		case NodeKind::Invert: ins.op = FieldOp::Invert; break;
		case NodeKind::Power:
			ins.op = FieldOp::Power; ins.params[0] = F.size() > 0 ? F[0] : 2;
			break;
		case NodeKind::Smoothstep:
			ins.op = FieldOp::Smoothstep;
			ins.params[0] = F.size() > 0 ? F[0] : 0;
			ins.params[1] = F.size() > 1 ? F[1] : 1;
			break;
		case NodeKind::Remap:
			ins.op = FieldOp::Remap;
			for (int k = 0; k < 4; ++k) ins.params[k] = F.size() > (size_t)k ? F[k] : (k < 2 ? (float)k : 0);
			break;
		default:
			out->error = std::string("'") + NodeKindName(n->kind) + "' is not a field node";
			return false;
		}
		// Binary vs unary wiring.
		if (n->kind == NodeKind::Add || n->kind == NodeKind::Mul
			|| n->kind == NodeKind::Min || n->kind == NodeKind::Max) {
			if (!need("a", a) || !need("b", b)) return false;
			ins.a = (uint32_t)a; ins.b = (uint32_t)b;
			return push();
		}
		if (!need("x", a)) return false;
		ins.a = (uint32_t)a;
		return push();
	}
};
} // namespace

CompileResult CompileGraph(const GraphDoc& doc, uint64_t graphId) {
	CompileResult r;
	// Exactly one Layer (MVP: single-layer preview).
	std::vector<uint32_t> layers;
	for (const auto& [id, n] : doc.nodes)
		if (n.kind == NodeKind::Layer) layers.push_back(id);
	if (layers.empty()) { r.error = "no Layer (output) node"; return r; }
	if (layers.size() > 1) { r.error = "more than one Layer node (MVP: one per graph)"; return r; }
	const PNode& layer = doc.nodes.at(layers[0]);
	// Domain (required).
	const PLink* dl = doc.inputLink(layer.id, "domain");
	if (!dl) { r.error = "Layer.domain is unlinked"; return r; }
	const PNode* dn = doc.find(dl->from);
	if (!dn) { r.error = "Layer.domain link dangles"; return r; }
	if (dn->kind == NodeKind::Grid) {
		r.domainKind = DomainKind::Grid;
		r.grid.origin = glm::vec3(dn->f.size() > 0 ? dn->f[0] : 0,
			dn->f.size() > 1 ? dn->f[1] : 0, dn->f.size() > 2 ? dn->f[2] : 0);
		r.grid.extent = glm::vec2(dn->f.size() > 3 ? dn->f[3] : 64,
			dn->f.size() > 4 ? dn->f[4] : 64);
		r.grid.cellsX = dn->f.size() > 5 ? std::max(1u, (uint32_t)dn->f[5]) : 8;
		r.grid.cellsZ = dn->f.size() > 6 ? std::max(1u, (uint32_t)dn->f[6]) : 8;
	}
	else if (dn->kind == NodeKind::Mesh) {
		r.domainKind = DomainKind::MeshSurface;
		int wn = dn->i.size() > 0 ? dn->i[0] : -1;
		if (wn < 0) { r.error = "Mesh node has no scene node bound (use 'Use selected')"; return r; }
		r.meshNode = (uint32_t)wn;
	}
	else if (dn->kind == NodeKind::Terrain) {
		r.domainKind = DomainKind::Heightfield;
		const auto& F = dn->f;
		r.terrain.origin = glm::vec2(F.size() > 0 ? F[0] : 0,
			F.size() > 1 ? F[1] : 0);
		r.terrain.size = F.size() > 2 ? std::max(1.0f, F[2]) : 64.0f;
		r.terrain.res = F.size() > 3
			? std::clamp((uint32_t)std::max(1.0f, F[3]), 8u, 256u) : 64;
		r.terrain.height = F.size() > 4 ? std::max(0.0f, F[4]) : 8.0f;
		r.terrain.freq = F.size() > 5 ? std::max(1e-4f, F[5]) : 0.02f;
		r.terrain.seed = dn->i.size() > 0 && dn->i[0] >= 0
			? (uint64_t)(uint32_t)dn->i[0] : 1;
		// Live-terrain binding (i[1]): a created terrain group world node.
		// When set, previews/evaluation sample the group's LIVE painted
		// grid (see RefreshPlacementPreview) instead of the analytic one.
		r.terrainBind = (dn->i.size() > 1 && dn->i[1] >= 0)
			? (uint32_t)dn->i[1] : UINT32_MAX;
	}
	else { r.error = "Layer.domain must come from a Grid, Mesh or Terrain node"; return r; }
	// Density expression (Accept preferred, field auto-wrapped at scale 1).
	const PLink* yl = doc.inputLink(layer.id, "density");
	if (!yl) { r.error = "Layer.density is unlinked"; return r; }
	const PNode* yn = doc.find(yl->from);
	if (!yn) { r.error = "Layer.density link dangles"; return r; }
	float acceptScale = 1.0f;
	uint32_t exprRoot = yl->from;
	if (yn->kind == NodeKind::Accept) {
		acceptScale = yn->f.size() > 0 ? yn->f[0] : 1.0f;
		const PLink* al = doc.inputLink(yn->id, "density");
		if (!al) { r.error = "Accept.density is unlinked"; return r; }
		exprRoot = al->from;
	}
	Ctx cx{ &doc, &r };
	int rootIdx = 0;
	if (!cx.emitField(exprRoot, rootIdx)) return r;
	r.program.graphSeed = 0; // filled below from Layer seed
	r.program.nodeId = 1;
	r.program.capabilities = PlacementCapability_CPU | PlacementCapability_GPU;
	// Fold the Accept scale into a trailing Mul (keeps one accept index).
	if (acceptScale != 1.0f) {
		FieldInstr c = OpConst(acceptScale);
		r.program.instrs.push_back(c);
		r.instrToNode.push_back((int)yn->id);
		FieldInstr m;
		m.op = FieldOp::Mul;
		m.a = (uint32_t)rootIdx;
		m.b = (uint32_t)(r.program.instrs.size() - 1);
		r.program.instrs.push_back(m);
		r.instrToNode.push_back((int)yn->id);
		rootIdx = (int)r.program.instrs.size() - 1;
	}
	r.program.acceptIndex = (uint32_t)rootIdx;
	// Prototypes (optional; kernel limit 8).
	const PLink* pl = doc.inputLink(layer.id, "protos");
	if (pl) {
		const PNode* pn = doc.find(pl->from);
		if (!pn || pn->kind != NodeKind::Prototypes) {
			r.error = "Layer.protos must come from a Prototypes node"; return r;
		}
		size_t n = pn->s.size();
		if (pn->f.size() < 2 + 2 * n) { r.error = "Prototypes node is malformed"; return r; }
		if (n == 0) { r.error = "Prototypes node needs at least one row"; return r; }
		if (n > 8) { r.error = "more than 8 prototypes (GPU kernel limit)"; return r; }
		r.config.slopeToRock = pn->f[0] != 0.0f;
		r.config.slopeThresholdDeg = pn->f[1];
		for (size_t k = 0; k < n; ++k) {
			Prototype p;
			p.assetUri = pn->s[k];
			p.weight = std::max(0.0f, pn->f[2 + 2 * k]);
			p.collisionRadius = std::max(0.05f, pn->f[2 + 2 * k + 1]);
			r.prototypes.add(p);
		}
	}
	else {
		Prototype p;
		p.assetUri = "Default";
		r.prototypes.add(p);
		r.warning = "no Prototypes linked: single default prototype";
	}
	// Transform (optional).
	const PLink* xl = doc.inputLink(layer.id, "xform");
	if (xl) {
		const PNode* xn = doc.find(xl->from);
		if (!xn || xn->kind != NodeKind::Transform) {
			r.error = "Layer.xform must come from a Transform node"; return r;
		}
		r.config.minScale = xn->f.size() > 0 ? xn->f[0] : 0.8f;
		r.config.maxScale = xn->f.size() > 1 ? xn->f[1] : 1.2f;
		if (r.config.maxScale < r.config.minScale)
			std::swap(r.config.minScale, r.config.maxScale);
	}
	// Spacing (optional).
	const PLink* sl = doc.inputLink(layer.id, "spacing");
	if (sl) {
		const PNode* sn = doc.find(sl->from);
		if (!sn || sn->kind != NodeKind::Spacing) {
			r.error = "Layer.spacing must come from a Min Distance node"; return r;
		}
		r.config.minSpacing = sn->f.size() > 0 ? std::max(0.0f, sn->f[0]) : 0.0f;
	}
	// Layer params: candidates + seed.
	uint32_t cands = layer.f.size() > 0 ? (uint32_t)layer.f[0] : 4096;
	r.config.candidatesPerTile = std::clamp(cands, 1u, 65536u);
	r.config.densityScale = 1.0f;
	r.seed = layer.s.size() > 0 ? StrU64(layer.s[0], 0) : 0;
	if (layer.s.empty() || layer.s[0].empty()) {
		r.error = "Layer seed is empty";
		return r;
	}
	r.program.graphSeed = r.seed ^ (graphId * 0x9e3779b97f4a7c15ull);
	r.ok = true;
	r.gpuOk = true; // MVP: every kind lowers to both backends
	return r;
}

// ---- EditorDoc ----------------------------------------------------------------------
GraphDoc* EditorDoc::current() {
	auto it = graphs.find(active);
	return it != graphs.end() ? &it->second : nullptr;
}
const GraphDoc* EditorDoc::current() const {
	auto it = graphs.find(active);
	return it != graphs.end() ? &it->second : nullptr;
}
uint64_t EditorDoc::addGraph(const std::string& name) {
	uint64_t id = nextGraph;
	while (graphs.contains(id)) ++id;
	nextGraph = id + 1;
	GraphDoc doc;
	doc.name = name.empty() ? ("Scatter " + std::to_string(id)) : name;
	BuildDefaultGraph(doc);
	graphs[id] = doc;
	active = id;
	return id;
}
void EditorDoc::pushUndo() {
	std::string snap;
	for (const auto& [gid, g] : graphs) {
		snap += "G " + std::to_string(gid) + " " + g.name + "\n";
		snap += SerializeGraph(g);
	}
	snap += "A " + std::to_string(active) + "\n";
	if (!undoStack.empty() && undoPos >= 0 && undoStack[(size_t)undoPos] == snap)
		return; // no change
	undoStack.resize((size_t)(undoPos + 1));
	undoStack.push_back(snap);
	if (undoStack.size() > 50) undoStack.erase(undoStack.begin());
	undoPos = (int)undoStack.size() - 1;
}
namespace {
// Restores snapshot text (G/A envelope + per-graph SerializeGraph blocks)
// into graphs/active/nextGraph. Returns false on corrupt input (leaves
// outputs untouched).
bool RestoreSnapshot(const std::string& snap, std::map<uint64_t, GraphDoc>& graphs,
	uint64_t& active, uint64_t& nextGraph) {
	std::map<uint64_t, GraphDoc> g;
	uint64_t act = 0;
	std::istringstream in(snap);
	std::string line, block;
	uint64_t gid = 0;
	bool inGraph = false;
	auto flush = [&]() {
		if (!inGraph) return;
		GraphDoc d;
		if (ParseGraph(block, d)) g[gid] = d;
		block.clear();
		inGraph = false;
	};
	while (std::getline(in, line)) {
		if (line.rfind("G ", 0) == 0) {
			flush();
			std::istringstream h(line.substr(2));
			if (!(h >> gid)) return false;
			std::string nm;
			std::getline(h, nm);
			while (!nm.empty() && nm.front() == ' ') nm.erase(nm.begin());
			block = "N " + nm + "\n"; // ParseGraph expects the N-name line
			inGraph = true;
		}
		else if (line.rfind("A ", 0) == 0) {
			flush();
			try { act = std::stoull(line.substr(2)); }
			catch (...) { return false; }
		}
		else if (inGraph && !line.empty()) { block += line + "\n"; }
	}
	flush();
	if (g.empty()) return false;
	graphs = g;
	active = g.contains(act) ? act : g.begin()->first;
	uint64_t mx = 0;
	for (const auto& [id, d] : graphs) mx = std::max(mx, id);
	nextGraph = mx + 1;
	return true;
}
} // namespace
bool EditorDoc::undo() {
	if (undoPos <= 0) return false;
	std::map<uint64_t, GraphDoc> g;
	uint64_t a = 0, ng = nextGraph;
	if (!RestoreSnapshot(undoStack[(size_t)(undoPos - 1)], g, a, ng)) return false;
	--undoPos;
	graphs = g;
	active = a;
	nextGraph = ng;
	preview.dirty = true;
	selected = -1;
	selectedLink = -1;
	return true;
}
bool EditorDoc::redo() {
	if (undoPos + 1 >= (int)undoStack.size()) return false;
	std::map<uint64_t, GraphDoc> g;
	uint64_t a = 0, ng = nextGraph;
	if (!RestoreSnapshot(undoStack[(size_t)(undoPos + 1)], g, a, ng)) return false;
	++undoPos;
	graphs = g;
	active = a;
	nextGraph = ng;
	preview.dirty = true;
	selected = -1;
	selectedLink = -1;
	return true;
}

// ---- EditorApp integration ----------------------------------------------------
// NOTE: EditorApp methods live in namespace editor (NOT editor::placement:
// C++ forbids defining editor::EditorApp members inside placement).
} // namespace editor::placement
