// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// ContentRegistry — see content_registry.h (the consolidated content
// architecture: the ONE content-resolution authority). The byte lanes
// mirror the legacy ProjectRuntime::ReadAsset split — an adopted
// backend's resident image first, else the SMP8 async pager (one
// positional page read, no whole-image preload) — with the provider
// walk unified so ids and paths agree about which version wins.

#include <Engine/Asset/content_registry.h>

#include <Engine/Asset/content_registry.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

namespace tour::asset {

namespace {

namespace fs = std::filesystem;

std::string HexId(uint64_t id) {
	std::ostringstream os;
	os << "0x" << std::hex << id;
	return os.str();
}

} // namespace

// ---------------------------------------------------------------------------
// kinds
// ---------------------------------------------------------------------------

bool CatalogKindToPakKind(uint8_t kind, smvk::PakAssetKind& out) {
	// The byte forms: TEXTURE (the .ktx2 envelope bytes), SOUND (the
	// raw .wav), SHADER (the raw compiled bytecode), AUDIOGRAPH (the
	// authored text, verbatim) and GI (the manifest rows ride Texture-
	// kind blobs — the .gi/* payload channel). SCENE and MESH are
	// identity-addressed (the world loader / the cluster) and have NO
	// VFS byte form.
	switch (kind) {
	case smvk::kCatalogKindTexture:
	case smvk::kCatalogKindGi:
		out = smvk::PakAssetKind::Texture;
		return true;
	case smvk::kCatalogKindSound:
		out = smvk::PakAssetKind::Sound;
		return true;
	case smvk::kCatalogKindShader:
		out = smvk::PakAssetKind::Shader;
		return true;
	case smvk::kCatalogKindStateMachine:
		out = smvk::PakAssetKind::StateMachine;
		return true;
	case smvk::kCatalogKindNavMesh:
		out = smvk::PakAssetKind::NavMesh;
		return true;
	case smvk::kCatalogKindShape:
		out = smvk::PakAssetKind::Shape;
		return true;
	case smvk::kCatalogKindSpriteSet:
		out = smvk::PakAssetKind::SpriteSet;
		return true;
	case smvk::kCatalogKindAudioGraph:
		out = smvk::PakAssetKind::AudioGraph;
		return true;
	default:
		return false;
	}
}

// ---------------------------------------------------------------------------
// overrides
// ---------------------------------------------------------------------------

bool ContentRegistry::AddPakOverride(const std::string& name,
	const std::string& pakPath, std::string& err) {
	err.clear();
	if (name.empty()) {
		err = "an override provider needs a name";
		return false;
	}
	if (ProviderByName(name)) {
		err = "a provider named '" + name + "' already exists";
		return false;
	}
	ContentProvider p;
	p.type = ContentProvider::Type::Pak;
	p.name = name;
	p.base = false;
	fs::path pp(pakPath);
	p.source = pp.lexically_normal().generic_string();
	m_overrides.push_back(std::move(p));
	return true;
}

bool ContentRegistry::AddLooseOverride(const std::string& name,
	const std::string& rootDir, std::string& err) {
	err.clear();
	if (name.empty()) {
		err = "an override provider needs a name";
		return false;
	}
	if (ProviderByName(name)) {
		err = "a provider named '" + name + "' already exists";
		return false;
	}
	ContentProvider p;
	p.type = ContentProvider::Type::Loose;
	p.name = name;
	p.base = false;
	p.loose = std::make_shared<smvk::LooseBackend>(rootDir);
	p.source = rootDir;
	m_overrides.push_back(std::move(p));
	return true;
}

bool ContentRegistry::AddBackendOverride(const std::string& name,
	std::shared_ptr<smvk::IVfsBackend> backend, std::string& err) {
	err.clear();
	if (name.empty() || !backend) {
		err = "an override provider needs a name and a backend";
		return false;
	}
	if (ProviderByName(name)) {
		err = "a provider named '" + name + "' already exists";
		return false;
	}
	ContentProvider p;
	p.type = ContentProvider::Type::Loose;
	p.name = name;
	p.base = false;
	p.loose = std::move(backend);
	p.source = backend->Name();
	m_overrides.push_back(std::move(p));
	return true;
}

// ---------------------------------------------------------------------------
// discovery (the .tapp-free lane)
// ---------------------------------------------------------------------------

bool ContentRegistry::DiscoverPak(const std::string& name,
	const std::string& pakPath, std::string& err,
	bool requireIdentityBlob) {
	err.clear();
	if (name.empty()) {
		err = "a discovered pak needs a provider name";
		return false;
	}
	// Idempotent: a provider with that name is already discovered.
	if (ProviderByName(name))
		return true;
	// The light SMP8 mount (header/footer/TOC — no blob bytes, no
	// whole image) + ONE page read for the identity table.
	auto pager = std::make_shared<smvk::PakPager>(pakPath);
	if (!pager->Valid()) {
		err = "the content pak '" + name
			+ "' failed its light mount: " + pager->OpenError();
		return false;
	}
	const smvk::PakTocEntry* entry = pager->Reader().Find(
		smvk::PakAssetKind::Identity, kPakAssetIdsPath);
	std::vector<PakAssetIdRow> rows;
	if (!entry) {
		if (requireIdentityBlob) {
			err = "the pak '" + name
				+ "' carries no identity metadata (the '#ids'"
				" registry blob) — recook it through the project"
				" build (or use the appmanifest compatibility lane)";
			return false;
		}
		// The LEGACY form: a pre-identity pak joins as a BASE
		// provider serving its paths — it contributes BYTES (the
		// path-only resolution lane), never identity rows.
	}
	else {
		std::string perr;
		smvk::PakPager::PinnedPage page = pager->Read(*entry, perr);
		if (!page) {
			err = "the pak '" + name + "' failed reading its identity"
				" table: " + perr;
			return false;
		}
		if (!ParseAssetIdsBlob(page.bytes(), rows, perr)) {
			err = "the pak '" + name + "' carries a malformed identity"
				" table: " + perr;
			return false;
		}
	}
	// Validate the WHOLE row set (against the existing table AND
	// within the pak's own rows) BEFORE committing anything — the
	// atomicity contract: a conflicting pak adds no records, no
	// provider, nothing.
	std::vector<ContentRecord> pending;
	pending.reserve(rows.size());
	std::unordered_map<uint64_t, size_t> pendingById;
	std::unordered_map<uint64_t, size_t> pendingByPath;
	for (const PakAssetIdRow& r : rows) {
		const std::string norm = smvk::vfs::Normalize(r.path);
		if (norm.empty()) {
			err = "the pak '" + name + "' identity row " + HexId(r.id)
				+ " path '" + r.path
				+ "' does not normalize to a valid logical path";
			return false;
		}
		const auto claimErr = [&](const std::string& what,
				const std::string& prevPak) -> std::string {
			if (prevPak == name)
				return "identity conflict inside '" + name + "': the "
					+ what + " is claimed twice by the same pak"
					" (recook)";
			return "identity conflict: the " + what
				+ " is claimed by both '" + prevPak + "' and '" + name
				+ "' (one owner per asset — a deployment conflict;"
				" recook)";
		};
		if (m_byId.count(r.id) || pendingById.count(r.id)) {
			const std::string prevPak = pendingById.count(r.id)
				? name : m_records[m_byId.at(r.id)].pak;
			err = claimErr("id " + HexId(r.id), prevPak);
			return false;
		}
		const uint64_t key = PathKey(r.kind, norm);
		if (m_byPath.count(key) || pendingByPath.count(key)) {
			const std::string prevPak = pendingByPath.count(key)
				? name : m_records[m_byPath.at(key)].pak;
			err = claimErr(std::string(smvk::CatalogKindToString(r.kind))
				+ " path '" + norm + "'", prevPak);
			return false;
		}
		ContentRecord rec;
		rec.id = r.id;
		rec.kind = r.kind;
		rec.path = norm;
		rec.pak = name;
		rec.sceneName = r.sceneName;
		pendingById.emplace(rec.id, pending.size());
		pendingByPath.emplace(key, pending.size());
		pending.push_back(std::move(rec));
	}
	// Commit: the provider first (the pager stays CACHED on it — the
	// discovery's own mount + page read already served), then the
	// records + both indexes.
	ContentProvider p;
	p.type = ContentProvider::Type::Pak;
	p.name = name;
	p.base = true;
	p.source = pakPath;
	p.pager = std::move(pager);
	p.pagerOpenTried = true;
	m_base.push_back(std::move(p));
	for (ContentRecord& rec : pending) {
		const size_t idx = m_records.size();
		m_byId.emplace(rec.id, idx);
		m_byPath.emplace(PathKey(rec.kind, rec.path), idx);
		m_records.push_back(std::move(rec));
	}
	return true;
}

// ---------------------------------------------------------------------------
// the convergent lookups
// ---------------------------------------------------------------------------

const ContentRecord* ContentRegistry::Find(uint64_t id) const {
	const auto it = m_byId.find(id);
	return it == m_byId.end() ? nullptr : &m_records[it->second];
}

const ContentRecord* ContentRegistry::Find(uint8_t kind,
	const std::string& path) const {
	const std::string norm = smvk::vfs::Normalize(path);
	if (norm.empty())
		return nullptr;
	const auto it = m_byPath.find(PathKey(kind, norm));
	return it == m_byPath.end() ? nullptr : &m_records[it->second];
}

// ---------------------------------------------------------------------------
// resolution
// ---------------------------------------------------------------------------

ContentRegistry::Resolution ContentRegistry::Resolve(uint64_t id,
	std::string& err) {
	err.clear();
	const ContentRecord* record = Find(id);
	if (!record) {
		err = "unknown content id " + HexId(id)
			+ " — not in this launch's content set (a version skew"
			" between the compiled project.h and the packaged"
			" content, or a pak this launch did not mount?)";
		return {};
	}
	return ResolveInternal(record, record->kind, record->path, err);
}

ContentRegistry::Resolution ContentRegistry::Resolve(uint8_t kind,
	const std::string& path, std::string& err) {
	err.clear();
	const std::string norm = smvk::vfs::Normalize(path);
	if (norm.empty()) {
		err = "the path '" + path
			+ "' does not normalize to a valid logical path";
		return {};
	}
	// The convergence contract: a path that carries a record of this
	// kind resolves through THAT record's identity (the same provider
	// selection the id form applies — an override serving the path
	// wins for both forms, never a split).
	const ContentRecord* record = Find(kind, norm);
	return ResolveInternal(record, kind, norm, err);
}

ContentRegistry::Resolution ContentRegistry::ResolveInternal(
	const ContentRecord* record, uint8_t pathKind,
	const std::string& norm, std::string& err) {
	// The kind that governs the probe: the RECORD's kind when one
	// exists (its path is the identity), else the path-only caller's.
	const uint8_t kind = record ? record->kind : pathKind;
	const std::string& path = record ? record->path : norm;

	// IDENTITY-ADDRESSED kinds (scene/mesh): no provider selection
	// applies — the payload rides the owning pak's cluster-internal
	// blobs (loaded by identity through the world loader), and byte
	// override semantics never made sense for them. The owner IS the
	// answer (null resolution + a loud refusal shape on Read).
	smvk::PakAssetKind pakKind = smvk::PakAssetKind::Texture;
	if (!CatalogKindToPakKind(kind, pakKind)) {
		if (!record)
			return {}; // path-only with a non-byte kind: nothing to probe
		const ContentProvider* owner = ProviderByName(record->pak);
		if (!owner) {
			err = "the scene/mesh record " + HexId(record->id)
				+ " names pak '" + record->pak
				+ "' which no provider carries";
			return {};
		}
		return Resolution{ record, owner, owner->IsOverride(),
			owner->generation };
	}

	// The provider walk: overrides (registration order) then base
	// (manifest order) — the first ENABLED provider that serves the
	// path under the kind is the effective provider.
	for (std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base }) {
		for (ContentProvider& p : *list) {
			if (!p.enabled)
				continue;
			if (!ProviderServes(p, path, pakKind))
				continue;
			return Resolution{ record, &p, p.IsOverride(),
				p.generation };
		}
	}
	// Nobody serves it. The corrupt-owner case names the pak's OWN
	// failure (actionable — a broken archive, never a vague miss);
	// otherwise the honest no-provider form.
	if (record) {
		if (const ContentProvider* owner = ProviderByName(record->pak)) {
			if (!owner->openError.empty()) {
				err = "the content pak '" + record->pak
					+ "' failed: " + owner->openError;
				return {};
			}
		}
		err = "no provider serves '" + path + "' (content id "
			+ HexId(record->id)
			+ " — a version skew between the build and the manifest,"
			" or a pak this launch did not mount?)";
	}
	else {
		err = "no provider serves '" + path + "'";
	}
	return {};
}

// ---------------------------------------------------------------------------
// the byte lanes
// ---------------------------------------------------------------------------

std::optional<std::vector<uint8_t>> ContentRegistry::Read(uint64_t id,
	std::string& err) {
	err.clear();
	const ContentRecord* record = Find(id);
	if (!record) {
		err = "unknown content id " + HexId(id)
			+ " — not in this launch's content set (a version skew"
			" between the compiled project.h and the packaged"
			" content, or a pak this launch did not mount?)";
		return std::nullopt;
	}
	// The kind gate (the same contract the runtime façade always
	// enforced): only TEXTURE, SOUND, SHADER and GI payloads are
	// VFS-addressed bytes; scenes load BY IDENTITY through the world
	// loader and meshes ship inside their scene's cluster.
	smvk::PakAssetKind pakKind = smvk::PakAssetKind::Texture;
	if (!CatalogKindToPakKind(record->kind, pakKind)) {
		err = "content id " + HexId(id) + " is a '"
			+ smvk::CatalogKindToString(record->kind)
			+ "' — only TEXTURE, SOUND, SHADER and GI payloads are"
			" VFS-addressed bytes (scenes load by identity through the"
			" world loader; meshes ship inside their scene's cluster)";
		return std::nullopt;
	}
	Resolution r = ResolveInternal(record, record->kind, record->path,
		err);
	if (!r.provider)
		return std::nullopt; // err composed by ResolveInternal
	// The resolution borrows const views; our own storage is mutable
	// (Read lazily opens provider pagers) — the contained const_cast.
	return ReadThrough(*const_cast<ContentProvider*>(r.provider),
		record->path, &pakKind, err);
}

std::optional<std::vector<uint8_t>> ContentRegistry::Read(uint8_t kind,
	const std::string& path, std::string& err) {
	err.clear();
	const std::string norm = smvk::vfs::Normalize(path);
	if (norm.empty()) {
		err = "the path '" + path
			+ "' does not normalize to a valid logical path";
		return std::nullopt;
	}
	smvk::PakAssetKind pakKind = smvk::PakAssetKind::Texture;
	if (!CatalogKindToPakKind(kind, pakKind)) {
		err = "the kind byte " + std::to_string(kind)
			+ " has no VFS-addressable byte form (scenes load by"
			" identity; meshes ship inside their scene's cluster)";
		return std::nullopt;
	}
	Resolution r = ResolveInternal(Find(kind, norm), kind, norm, err);
	if (!r.provider)
		return std::nullopt;
	return ReadThrough(*const_cast<ContentProvider*>(r.provider), norm,
		&pakKind, err);
}

std::optional<std::vector<uint8_t>> ContentRegistry::ReadPath(
	const std::string& path, std::string& err) {
	err.clear();
	const std::string norm = smvk::vfs::Normalize(path);
	if (norm.empty()) {
		err = "the path '" + path
			+ "' does not normalize to a valid logical path";
		return std::nullopt;
	}
	// A path with NO catalog identity (mod-added content, loose
	// files): the same provider precedence, probing every
	// VFS-addressable kind per provider (texture first — the payload
	// channel's dominant kind).
	for (std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base }) {
		for (ContentProvider& p : *list) {
			if (!p.enabled)
				continue;
			for (const smvk::PakAssetKind kind :
					{ smvk::PakAssetKind::Texture,
						smvk::PakAssetKind::Sound,
						smvk::PakAssetKind::Shader,
						smvk::PakAssetKind::AudioGraph,
						smvk::PakAssetKind::StateMachine,
						smvk::PakAssetKind::NavMesh }) {
				if (!ProviderServes(p, norm, kind))
					continue;
				return ReadThrough(p, norm, &kind, err);
			}
		}
	}
	err = "no provider serves '" + norm + "'";
	return std::nullopt;
}

// ---------------------------------------------------------------------------
// backend adoption (the shared-image lane)
// ---------------------------------------------------------------------------

bool ContentRegistry::AdoptBackend(const std::string& pakFile,
	std::shared_ptr<smvk::PakBackend> backend) {
	// First adoption wins per archive contract (the cache's own
	// open and an adopted host backend are the same archive by
	// construction); the resident image releases the provider's
	// cached pager (one worker + handle per archive back) and FULLY
	// RESETS the pager-open state — a later explicit PagerFor (the
	// compiled-plan lane's TOC folding) lazily opens a fresh one.
	if (pakFile.empty() || !backend)
		return false;
	ContentProvider* p = ProviderByName(pakFile);
	if (!p || p->type != ContentProvider::Type::Pak)
		return false;
	if (p->backend)
		return true;
	p->backend = std::move(backend);
	p->openError.clear();
	p->pager.reset();
	p->pagerOpenTried = false;
	return true;
}

bool ContentRegistry::HasBackend(const std::string& pakFile) const {
	const ContentProvider* p = ProviderByName(pakFile);
	return p && p->type == ContentProvider::Type::Pak && p->backend;
}

std::shared_ptr<smvk::PakBackend> ContentRegistry::BackendFor(
	const std::string& pakFile) const {
	const ContentProvider* p = ProviderByName(pakFile);
	return p && p->type == ContentProvider::Type::Pak
		? p->backend : nullptr;
}

std::shared_ptr<smvk::PakPager> ContentRegistry::PagerFor(
	const std::string& pakFile, std::string& err) {
	err.clear();
	ContentProvider* p = ProviderByName(pakFile);
	if (!p || p->type != ContentProvider::Type::Pak) {
		err = "no pak provider named '" + pakFile + "'";
		return nullptr;
	}
	// The cached pager first (byte-lane opens + a previous explicit
	// request share ONE instance per archive while no backend holds
	// the slot).
	if (p->pager)
		return p->pager;
	// Open (or REOPEN — a backend adoption released the cached
	// instance): the explicit accessor answers a usable pager even
	// when the image lane is the byte path (the plan lane folds TOCs
	// through it, never pages payloads).
	auto pager = std::make_shared<smvk::PakPager>(p->source);
	p->pagerOpenTried = true;
	if (pager->Valid()) {
		p->pager = std::move(pager);
		return p->pager;
	}
	// The failure records the provider's openError (the corrupt-owner
	// diagnostic feeds on it) + answers null + err.
	p->openError = pager->OpenError();
	err = "the content pak '" + pakFile + "' failed: " + p->openError;
	return nullptr;
}

// ---------------------------------------------------------------------------
// mod toggling
// ---------------------------------------------------------------------------

bool ContentRegistry::SetProviderEnabled(const std::string& name,
	bool enabled) {
	ContentProvider* p = ProviderByName(name);
	if (!p || p->enabled == enabled)
		return false;
	p->enabled = enabled;
	++p->generation; // a cached resolution's invalidation signal
	return true;
}

bool ContentRegistry::ProviderEnabled(const std::string& name) const {
	const ContentProvider* p = ProviderByName(name);
	return p && p->enabled;
}

uint64_t ContentRegistry::ProviderGeneration(
	const std::string& name) const {
	const ContentProvider* p = ProviderByName(name);
	return p ? p->generation : 0;
}

// ---------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------

std::vector<std::string> ContentRegistry::ProvidersFor(uint8_t kind,
	const std::string& path) {
	std::vector<std::string> out;
	smvk::PakAssetKind pakKind = smvk::PakAssetKind::Texture;
	if (!CatalogKindToPakKind(kind, pakKind))
		return out;
	const std::string norm = smvk::vfs::Normalize(path);
	if (norm.empty())
		return out;
	for (std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base }) {
		for (ContentProvider& p : *list) {
			if (!p.enabled)
				continue;
			if (ProviderServes(p, norm, pakKind))
				out.push_back(p.name);
		}
	}
	return out;
}

std::vector<std::string> ContentRegistry::ListPathsUnder(
	const std::string& prefix) {
	// The union across every ENABLED provider, deduplicated + sorted
	// (OverlayVfs::ListDir's semantics): the child path "prefix/<next
	// segment>" per provider — a loose tree lists its real children,
	// a pak its VFS-addressable TOC entries. The anim-clip fallback's
	// legacy hunt + tooling; one level, prefixed, read-only.
	const std::string norm = smvk::vfs::Normalize(prefix);
	if (norm.empty())
		return {};
	const std::string prefixSlash = norm + "/";
	std::vector<std::string> out;
	const auto addPakReader = [&](const PakReader& reader) {
		// The VFS-addressable kinds (pak.h's interop contract) — the
		// same set PakBackend::ListDir unions.
		for (const PakAssetKind kind :
				{ PakAssetKind::Texture, PakAssetKind::Sound,
					PakAssetKind::Shader,
					PakAssetKind::AudioGraph, PakAssetKind::StateMachine,
					PakAssetKind::NavMesh }) {
			for (const PakTocEntry* e : reader.BlobsOfKind(kind)) {
				const std::string& p = e->path;
				if (p.size() <= prefixSlash.size()
						|| p.compare(0, prefixSlash.size(), prefixSlash)
							!= 0)
					continue;
				const std::string rest = p.substr(prefixSlash.size());
				const size_t slash = rest.find('/');
				out.push_back(slash == std::string::npos
					? p : prefixSlash + rest.substr(0, slash));
			}
		}
	};
	for (std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base }) {
		for (ContentProvider& p : *list) {
			if (!p.enabled)
				continue;
			switch (p.type) {
			case ContentProvider::Type::Loose:
				if (p.loose)
					for (const smvk::VfsPath& e :
							p.loose->ListDir(norm))
						out.push_back(e);
				break;
			case ContentProvider::Type::Pak:
				if (p.backend)
					addPakReader(p.backend->Reader());
				else if (smvk::PakPager* pager = PagerOf(p))
					addPakReader(pager->Reader());
				break;
			}
		}
	}
	std::sort(out.begin(), out.end());
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

std::vector<ContentRegistry::ProviderInfo>
ContentRegistry::ProviderInfos() const {
	std::vector<ProviderInfo> out;
	out.reserve(m_overrides.size() + m_base.size());
	const auto push = [&](const ContentProvider& p) {
		ProviderInfo info;
		info.name = p.name;
		info.source = p.source;
		info.type = p.type;
		info.base = p.base;
		info.enabled = p.enabled;
		out.push_back(std::move(info));
	};
	for (const ContentProvider& p : m_overrides)
		push(p);
	for (const ContentProvider& p : m_base)
		push(p);
	return out;
}

// ---------------------------------------------------------------------------
// internals
// ---------------------------------------------------------------------------

ContentProvider* ContentRegistry::ProviderByName(
	const std::string& name) {
	for (std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base })
		for (ContentProvider& p : *list)
			if (p.name == name)
				return &p;
	return nullptr;
}

const ContentProvider* ContentRegistry::ProviderByName(
	const std::string& name) const {
	for (const std::deque<ContentProvider>* list :
			{ &m_overrides, &m_base })
		for (const ContentProvider& p : *list)
			if (p.name == name)
				return &p;
	return nullptr;
}

smvk::PakPager* ContentRegistry::PagerOf(ContentProvider& p) {
	if (p.pager)
		return p.pager.get();
	// Loud once: a failed light mount records the failure + answers
	// "not serving" from then on (the corrupt-owner error surfaces
	// through ResolveInternal when nothing serves the path).
	if (p.pagerOpenTried)
		return nullptr;
	p.pagerOpenTried = true;
	auto pager = std::make_shared<smvk::PakPager>(p.source);
	if (pager->Valid()) {
		p.pager = std::move(pager);
		return p.pager.get();
	}
	p.openError = pager->OpenError();
	return nullptr;
}

bool ContentRegistry::ProviderServes(ContentProvider& p,
	const std::string& norm, smvk::PakAssetKind kind) {
	switch (p.type) {
	case ContentProvider::Type::Loose:
		return p.loose && p.loose->Exists(norm);
	case ContentProvider::Type::Pak:
		break;
	}
	// The resident-backend lane first (the whole image the host
	// preloaded / the scene lane opened): one copy of the bytes,
	// whatever opened it first.
	if (p.backend)
		return p.backend->Reader().Find(kind, norm) != nullptr;
	// The pager lane: the SMP8 light mount's TOC probe (no blob
	// bytes, no preload — the pager reads pages on demand).
	smvk::PakPager* pager = PagerOf(p);
	return pager && pager->Reader().Find(kind, norm) != nullptr;
}

std::optional<std::vector<uint8_t>> ContentRegistry::ReadThrough(
	ContentProvider& p, const std::string& norm,
	const smvk::PakAssetKind* kind, std::string& err) {
	err.clear();
	if (p.type == ContentProvider::Type::Loose)
		return p.loose ? p.loose->TryRead(norm) : std::nullopt;
	// The resident backend serves through its VFS surface (the
	// kind-appropriate bytes for the entry — PakBackend::TryRead).
	if (p.backend)
		return p.backend->TryRead(norm);
	smvk::PakPager* pager = PagerOf(p);
	if (!pager) {
		err = "the content pak '" + p.name + "' failed: "
			+ p.openError;
		return std::nullopt;
	}
	// The pager lane: the entry by kind (or any VFS-addressable
	// kind for the path-only form), ONE positional page read.
	const smvk::PakTocEntry* entry = nullptr;
	smvk::PakAssetKind found = smvk::PakAssetKind::Texture;
	if (kind) {
		entry = pager->Reader().Find(*kind, norm);
		found = *kind;
	}
	else {
		for (const smvk::PakAssetKind probe :
				{ smvk::PakAssetKind::Texture,
					smvk::PakAssetKind::Sound,
					smvk::PakAssetKind::Shader,
					smvk::PakAssetKind::AudioGraph,
						smvk::PakAssetKind::StateMachine,
						smvk::PakAssetKind::NavMesh }) {
			entry = pager->Reader().Find(probe, norm);
			if (entry) {
				found = probe;
				break;
			}
		}
	}
	if (!entry)
		return std::nullopt; // a miss: the caller composes the form
	std::string perr;
	smvk::PakPager::PinnedPage page = pager->Read(*entry, perr);
	if (!page) {
		err = perr; // a corrupt page names its path — loud + terminal
		return std::nullopt;
	}
	return smvk::PakServeVfsBytes(found, page.bytes(), norm);
}

uint64_t ContentRegistry::PathKey(uint8_t kind,
	const std::string& norm) const {
	// FNV-1a64 over the normalized path (the same fold PakHashPath
	// applies), the kind byte folded in — one hash space per kind.
	uint64_t h = PakHashPath(norm);
	h ^= kind;
	h *= 0x100000001b3ULL;
	return h;
}

uint64_t ContentRegistry::RecordFingerprint() const {
	// The identity fold in CANONICAL emission order: kind-bucketed
	// (the generator's bucket sequence — scene..gi), id-sorted within
	// each bucket. The records' INSERTION order is whatever lane
	// built the table (manifest adoption rows, per-pak discovery
	// rows) — the fingerprint is over IDENTITY, and identity's order
	// is the emission sequence, never the build order.
	std::vector<const ContentRecord*> ordered;
	ordered.reserve(m_records.size());
	for (const ContentRecord& r : m_records)
		ordered.push_back(&r);
	std::stable_sort(ordered.begin(), ordered.end(),
		[](const ContentRecord* x, const ContentRecord* y) {
			if (x->kind != y->kind) {
				// The generator's bucket order: scene, texture,
				// mesh, sound, shader, gi, audiograph (the
				// kCatalogKind* ABI).
				return x->kind < y->kind;
			}
			return x->id < y->id;
		});
	std::vector<uint64_t> ids;
	std::vector<uint8_t> kinds;
	std::vector<const char*> paths;
	ids.reserve(ordered.size());
	kinds.reserve(ordered.size());
	paths.reserve(ordered.size());
	for (const ContentRecord* r : ordered) {
		ids.push_back(r->id);
		kinds.push_back(r->kind);
		paths.push_back(r->path.c_str());
	}
	return smvk::ComputeCatalogFingerprint(ids.data(), kinds.data(),
		paths.data(), ids.size());
}

} // namespace tour::asset
