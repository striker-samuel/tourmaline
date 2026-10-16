// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

#pragma once
// StoreAdapter: gltf_import's GltfArtifactCache over the registry's
// ArtifactStore. The editor and PakCooker inject this into their import
// paths so texture transcodes accelerate through the registry
// (hash-validated copy on hit; stage-into-registry on miss). The store
// stays the single owner of staging/flushing; the adapter only maps
// the interface.
//
// Key scoping (editor only): SetKeyScope prefixes every texture uri
// with the importing top-level model's asset GUID ("<guid>/<uri>"), so
// two models carrying identically-NAMED textures never thrash the one
// (klass, uri) row (latest-wins upsert + retranscode ping-pong).
// Nested reference imports inherit the top model's scope — consistent
// across sessions (cache still hits; only namespaced differently than
// the same model imported standalone). Unscoped (cooker, or editor
// before a project) keeps legacy bare-uri keys.

#include <string>
#include <vector>

#include <gltf_import/gltf.h>
#include <artifact_cache/artifact_cache.h>

class StoreArtifactCacheAdapter final : public GltfArtifactCache {
public:
	explicit StoreArtifactCacheAdapter(smvk::cache::ArtifactStore& store)
		: m_store(store) {
	}

	// Scope prefix (see header comment): empty = legacy bare-uri keys.
	void SetKeyScope(const std::string& scope) { m_scope = scope; }
	const std::string& KeyScope() const { return m_scope; }

	bool Get(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out) override {
		std::string err;
		return m_store.Get(klass, Scoped(uri), out, err);
	}

	bool Put(const std::string& klass, const std::string& uri,
		const void* payload, size_t payloadSize,
		const std::string& sourceHash, const std::string& meta) override {
		std::string err;
		return m_store.Put(klass, Scoped(uri),
			std::span<const uint8_t>(
				static_cast<const uint8_t*>(payload), payloadSize),
			sourceHash, meta, err);
	}

	bool Has(const std::string& klass, const std::string& uri,
		const std::string& sourceHash) override {
		// Registry-only exact-hash probe: the stale-hit gate. False
		// when unknown OR the row was staged from different source
		// bytes (edited image / another model's same-named texture).
		return m_store.Has(klass, Scoped(uri), sourceHash);
	}

private:
	// "<scope>/<uri>" when scoped; uri unchanged otherwise (legacy
	// rows keep serving).
	std::string Scoped(const std::string& uri) const {
		if (m_scope.empty())
			return uri;
		return m_scope + "/" + uri;
	}

	smvk::cache::ArtifactStore& m_store;
	std::string m_scope; // importing model's asset GUID ("" = legacy)
};
