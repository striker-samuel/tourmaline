#pragma once
// StoreAdapter: gltf_import's GltfArtifactCache over the scratch-disk
// ArtifactStore. The editor and PakCooker inject this into their import
// paths so texture transcodes accelerate through the scratch registry
// (hash-validated copy on hit; stage-into-scratch on miss). The store
// stays the single owner of staging/flushing; the adapter only maps
// the interface.

#include <string>
#include <vector>

#include <gltf_import/gltf.h>
#include <artifact_cache/artifact_cache.h>

class StoreArtifactCacheAdapter final : public GltfArtifactCache {
public:
	explicit StoreArtifactCacheAdapter(smvk::cache::ArtifactStore& store)
		: m_store(store) {
	}

	bool Get(const std::string& klass, const std::string& uri,
		std::vector<uint8_t>& out) override {
		std::string err;
		return m_store.Get(klass, uri, out, err);
	}

	bool Put(const std::string& klass, const std::string& uri,
		const void* payload, size_t payloadSize,
		const std::string& sourceHash, const std::string& meta) override {
		std::string err;
		return m_store.Put(klass, uri,
			std::span<const uint8_t>(
				static_cast<const uint8_t*>(payload), payloadSize),
			sourceHash, meta, err);
	}

private:
	smvk::cache::ArtifactStore& m_store;
};
