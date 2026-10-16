// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// Maya-style dependency-graph core for the EDITOR runtime + tour format.
//
// The Maya split (the architecture this mirrors, in API form):
//
//   Node TYPE          = attribute DEFINITIONS (schema): the static
//                        MObject / initialize() / addAttribute() layer.
//   Node INSTANCE      = a scene node: plugs + values.
//   Plug               = (instance, attribute) endpoint — the MPlug.
//   Connection         = plug -> plug — DG topology (NEVER
//                        attribute->attribute: that would connect every
//                        instance of the type).
//   attributeAffects   = intra-node DIRTY metadata ("if src changes,
//                        dst is stale") — not dataflow.
//   DataBlock          = per-instance value storage (MDataBlock); the
//                        typed accessors here are the MDataHandle side.
//   Internal attribute = kAttrInternal: the owning implementation
//                        intercepts reads/writes instead of the
//                        generic storage path.
//
// Scope: EDITOR-ONLY. The engine (clusters/VFS/runtime) is untouched —
// this header has no engine includes. The placement graph is the first
// consumer: its node kinds register schemas here, its links are
// Connections, its f/i/s vectors are the DataBlock, and its tour lines
// serialize ATTRIBUTES BY NAME (with legacy positional parsing).
//
// Attribute definitions are process-global (per TYPE, not per node):
// exactly like Maya's static MObject members — a FooNode::aInput never
// holds a value; foo1.input / foo2.input are Plug endpoints whose
// values live in each instance's DataBlock.

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace editor::dg {

// ---- attribute descriptors ------------------------------------------------

enum class AttrType : uint8_t { Double = 0, Int, String };

// Semantic connection classes (placement pin types). Channel::None =
// a plain authored value (never connected).
enum class Channel : uint8_t { None = 0, Domain, Field, Protos, Xform, Spacing };

enum AttrFlagBits : uint32_t {
	kAttrReadable = 1u << 0,   // plug can drive a connection (source)
	kAttrWritable = 1u << 1,   // plug can receive a connection (dest)
	kAttrConnectable = 1u << 2, // accepts connections at all
	kAttrKeyable = 1u << 3,    // authored in the UI (params)
	kAttrStorable = 1u << 4,  // persists to the tour
	kAttrHidden = 1u << 5,    // visibility (independent of internal!)
	kAttrInternal = 1u << 6,  // storage intercepted by the implementation
	kAttrOutput = 1u << 7,    // output-side plug of the type
};

// Attribute DEFINITION (the static-MObject layer). One per (type,
// name); values live per instance. `slot` is the index into the owning
// instance's typed storage (the placement PNode f/i/s vectors — the
// DataBlock).
struct AttributeDef {
	const char* longName = "";
	const char* shortName = "";
	AttrType type = AttrType::Double;
	Channel channel = Channel::None;
	uint32_t flags = kAttrReadable | kAttrWritable | kAttrKeyable
		| kAttrStorable;
	double defDouble = 0.0;
	int64_t defInt = 0;
	const char* defString = "";
	uint32_t slot = 0;
	bool is(uint32_t f) const { return (flags & f) == f; }
};

// Builders (the nAttr.create(...) flavor).
inline AttributeDef Num(const char* name, const char* shortName,
	double def, uint32_t flags = kAttrKeyable | kAttrStorable
		| kAttrReadable | kAttrWritable) {
	AttributeDef a{};
	a.longName = name;
	a.shortName = shortName;
	a.type = AttrType::Double;
	a.defDouble = def;
	a.flags = flags;
	return a;
}
inline AttributeDef Int(const char* name, const char* shortName,
	int64_t def, uint32_t flags = kAttrKeyable | kAttrStorable
		| kAttrReadable | kAttrWritable) {
	AttributeDef a{};
	a.longName = name;
	a.shortName = shortName;
	a.type = AttrType::Int;
	a.defInt = def;
	a.flags = flags;
	return a;
}
inline AttributeDef Str(const char* name, const char* shortName,
	const char* def, uint32_t flags = kAttrKeyable | kAttrStorable
		| kAttrReadable | kAttrWritable) {
	AttributeDef a{};
	a.longName = name;
	a.shortName = shortName;
	a.type = AttrType::String;
	a.defString = def;
	a.flags = flags;
	return a;
}
// Pin builder: an input or output plug attr (connectable; authored
// values ride the DataBlock for input pins' defaults).
inline AttributeDef Pin(const char* name, const char* shortName,
	Channel ch, bool output) {
	AttributeDef a{};
	a.longName = name;
	a.shortName = shortName;
	a.type = AttrType::Double;
	a.channel = ch;
	a.flags = kAttrReadable | kAttrWritable | kAttrConnectable
		| (output ? kAttrOutput : 0);
	return a;
}

// ---- node types (schema + attributeAffects) --------------------------------

struct NodeType {
	std::string name;     // unique type name ("placement.density")
	std::string category; // UI grouping ("Field")
	std::vector<AttributeDef> attrs; // slot order = storage order
	// attributeAffects(src, dst): dirty metadata, NOT dataflow.
	std::vector<std::pair<std::string, std::string>> affects;
	const AttributeDef* findAttr(const char* name) const;
	const AttributeDef* findInput(const char* name) const; // !kAttrOutput
	const AttributeDef* findOutput(const char* name) const; // kAttrOutput
};

// Registration is the initialize() pattern: idempotent per type name;
// slots are (re)assigned in declaration order on register. Definitions
// are process-global and never per-instance.
const NodeType* RegisterType(NodeType type);
const NodeType* FindType(const std::string& name);

// ---- plugs + connections ----------------------------------------------------

// Plug = (node instance, attribute definition). The INSTANCE endpoint:
// foo1.input and foo2.input share one AttributeDef, differ by node.
struct Plug {
	uint32_t node = 0;
	const AttributeDef* attr = nullptr;
	bool valid() const { return attr != nullptr; }
	bool operator==(const Plug& o) const {
		return node == o.node && attr == o.attr;
	}
};
struct PlugHash {
	size_t operator()(const Plug& p) const {
		return std::hash<uint64_t>()(
			(uint64_t(p.node) << 32)
			^ uint64_t(reinterpret_cast<uintptr_t>(p.attr)));
	}
};

// Connection = source plug -> dest plug (DG topology lives on PLUGS).
struct Connection {
	Plug src;
	Plug dst;
};

// Connection semantics (Maya): src must be readable+connectable, dst
// writable+connectable, and the CHANNEL must match. Never
// attribute->attribute — always per-instance plugs.
bool CanConnect(const Plug& src, const Plug& dst);

// ---- dirty propagation ------------------------------------------------------

// attributeAffects made executable: seed a changed input plug; affected
// outputs mark dirty; connections carry dirtiness into downstream
// inputs; repeat to fixpoint. (Maya marks DEPENDENTS dirty — this walk
// never goes upstream.)
class DirtySet {
public:
	// Seed + propagate to fixpoint. typeOf resolves a node's schema;
	// conns supplies the graph topology.
	void mark(uint32_t node, const AttributeDef* attr,
		const std::function<const NodeType*(uint32_t)>& typeOf,
		const std::vector<Connection>& conns);
	bool dirty(uint32_t node, const AttributeDef* attr) const;
	bool dirtyNode(uint32_t node) const; // any plug of the node dirty
	void clear();
	size_t size() const { return m_dirty.size(); }
private:
	void markPlug(const Plug& p,
		const std::function<const NodeType*(uint32_t)>& typeOf,
		const std::vector<Connection>& conns,
		std::unordered_set<Plug, PlugHash>& visited);
	std::unordered_set<Plug, PlugHash> m_dirty;
};

// ---- DataBlock side -----------------------------------------------------------

// The VALUE storage stays per-instance and consumer-owned (the placement
// PNode's f/i/s vectors ARE the DataBlock). The contract:
// AttributeDef::slot is the index into the instance's typed vector for
// the attribute's AttrType; defaults come from the definition. Consumers
// implement typed access over their storage (see editor_placement_dg.cpp:
// GetAttr/SetAttr over PNode). Internal attributes (kAttrInternal) route
// through the owning implementation instead of the generic storage.

} // namespace editor::dg
