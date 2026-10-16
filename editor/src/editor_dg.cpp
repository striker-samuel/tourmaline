// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// editor_dg.cpp — Maya-style dependency-graph core (see editor_dg.h
// for the architecture notes). Schema registry, plug semantics,
// connection validation, dirty propagation. Editor-only: no engine
// state is touched by anything in this TU.

#include "editor_dg.h"

#include <mutex>

namespace editor::dg {

const AttributeDef* NodeType::findAttr(const char* name) const {
	for (const AttributeDef& a : attrs)
		if (std::string(a.longName) == name)
			return &a;
	return nullptr;
}
const AttributeDef* NodeType::findInput(const char* name) const {
	const AttributeDef* a = findAttr(name);
	return (a && !a->is(kAttrOutput)) ? a : nullptr;
}
const AttributeDef* NodeType::findOutput(const char* name) const {
	const AttributeDef* a = findAttr(name);
	return (a && a->is(kAttrOutput)) ? a : nullptr;
}

// Single process-global registry (attribute definitions belong to the
// TYPE, not to instances — the static-MObject lifetime).
static std::unordered_map<std::string, NodeType>& Registry() {
	static std::unordered_map<std::string, NodeType>* registry = []() {
		return new std::unordered_map<std::string, NodeType>();
	}();
	return *registry;
}

const NodeType* RegisterType(NodeType type) {
	static std::mutex mu;
	std::lock_guard<std::mutex> lock(mu);
	auto& registry = Registry();
	auto it = registry.find(type.name);
	if (it != registry.end())
		return &it->second;
	// Slots: per-AttrType declaration order over STORABLE attributes
	// only — plugs/pins (connectable, not storable) hold no values and
	// take no storage (the DataBlock contract; slot layout matches the
	// historical positional vectors).
	uint32_t dbl = 0, ints = 0, str = 0;
	for (AttributeDef& a : type.attrs) {
		if (!a.is(kAttrStorable))
			continue;
		switch (a.type) {
		case AttrType::Double: a.slot = dbl++; break;
		case AttrType::Int: a.slot = ints++; break;
		case AttrType::String: a.slot = str++; break;
		}
	}
	const std::string key = type.name;
	return &registry.emplace(key, std::move(type)).first->second;
}

const NodeType* FindType(const std::string& name) {
	static std::mutex mu;
	std::lock_guard<std::mutex> lock(mu);
	auto& registry = Registry();
	auto it = registry.find(name);
	return it != registry.end() ? &it->second : nullptr;
}

bool CanConnect(const Plug& src, const Plug& dst) {
	if (!src.valid() || !dst.valid() || src.node == dst.node)
		return false;
	if (!src.attr->is(kAttrConnectable) || !src.attr->is(kAttrReadable))
		return false;
	if (!dst.attr->is(kAttrConnectable) || !dst.attr->is(kAttrWritable))
		return false;
	if (!src.attr->is(kAttrOutput) || dst.attr->is(kAttrOutput))
		return false; // source = output side, dest = input side
	return src.attr->channel == dst.attr->channel;
}

void DirtySet::mark(uint32_t node, const AttributeDef* attr,
	const std::function<const NodeType*(uint32_t)>& typeOf,
	const std::vector<Connection>& conns) {
	std::unordered_set<Plug, PlugHash> visited;
	markPlug({ node, attr }, typeOf, conns, visited);
}

void DirtySet::markPlug(const Plug& p,
	const std::function<const NodeType*(uint32_t)>& typeOf,
	const std::vector<Connection>& conns,
	std::unordered_set<Plug, PlugHash>& visited) {
	if (!p.valid() || !visited.insert(p).second)
		return; // fixpoint: each plug visited once
	m_dirty.insert(p);
	const NodeType* t = typeOf(p.node);
	if (!t)
		return;
	// attributeAffects: the changed plug dirties affected plugs of the
	// SAME instance (dirty metadata, not dataflow).
	for (const auto& [src, dst] : t->affects) {
		if (std::string(p.attr->longName) != src)
			continue;
		if (const AttributeDef* out = t->findAttr(dst.c_str()))
			markPlug({ p.node, out }, typeOf, conns, visited);
	}
	// Connections: an output source carries dirtiness into its
	// destination plug on another instance (downstream only — Maya
	// marks dependents, never upstream).
	for (const Connection& c : conns)
		if (c.src == p)
			markPlug(c.dst, typeOf, conns, visited);
}

bool DirtySet::dirty(uint32_t node, const AttributeDef* attr) const {
	return m_dirty.contains(Plug{ node, attr });
}

bool DirtySet::dirtyNode(uint32_t node) const {
	for (const Plug& p : m_dirty)
		if (p.node == node)
			return true;
	return false;
}

void DirtySet::clear() {
	m_dirty.clear();
}

} // namespace editor::dg
