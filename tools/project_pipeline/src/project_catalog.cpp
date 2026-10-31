// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project.h generator (see project_catalog.h). Three passes:
// identity (every plan entity's GUID — sidecar read or mint+adopt),
// registry (EnsureContentId + the tombstone sweep), emission (the
// constexpr header text, byte-deterministic for the same catalog).

#include <project_pipeline/project_catalog.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

#include <Engine/Asset/appmanifest.h>
#include <Engine/Asset/tour.h>

#include <artifact_cache/artifact_cache.h>

#include <gltf_import/asset_export.h>

namespace smvk::project {

namespace {

namespace fs = std::filesystem;

// The EOF metadata section's format version (both markers carry it; a
// mismatching version reads as ABSENT — the caller scans fresh, never
// reuses a cache it cannot understand).
constexpr int kMetadataVersion = 1;

const char* const kMetadataBegin = "TOURMALINE-METADATA-BEGIN";
const char* const kMetadataEnd = "TOURMALINE-METADATA-END";

// FNV-1a 64 (the repo's stable hash shape).
uint64_t Fnv(const char* s) {
	uint64_t h = 1469598103934665603ULL;
	while (*s) {
		h ^= uint64_t(uint8_t(*s++));
		h *= 1099511628211ULL;
	}
	return h;
}

// 16 lowercase hex digits (the fingerprint string shape — matches the
// artifact cache's HashBytes output).
std::string Hex64(uint64_t v) {
	char buf[17];
	std::snprintf(buf, sizeof buf, "%016llx",
		static_cast<unsigned long long>(v));
	return std::string(buf);
}

// C++ string-literal escaping for the emitted header (the generator is
// a compiler front end: a path or pak name carrying a quote or a
// backslash must never break the emitted code).
std::string CppStr(const std::string& s) {
	std::string o;
	for (char c : s) {
		if (c == '\\' || c == '"')
			o += '\\';
		o += c;
	}
	return o;
}

// Read a whole (small) text file. A missing file is not an error —
// `exists` says which.
bool ReadTextFile(const std::string& path, bool& exists,
	std::string& out) {
	out.clear();
	exists = false;
	std::error_code ec;
	if (!fs::exists(fs::path(path), ec))
		return true;
	exists = true;
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return false;
	out.assign((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	return in.good() || in.eof();
}

// file_time_type -> unix seconds (the EOF stores mtime_s= — explicit
// unit, explicit epoch). clock_cast is THE C++20 cross-clock
// conversion (file_time_type::clock::to_sys is not a guaranteed
// member — MSVC's _File_time_clock doesn't expose it).
int64_t ToUnixSeconds(const fs::file_time_type& t) {
	return int64_t(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::clock_cast<std::chrono::system_clock>(t)
			.time_since_epoch())
		.count());
}

// ---- the EOF metadata text format ----------------------------------
// Every record line is a `// `-prefixed comment between the versioned
// markers. Two line shapes:
//   keyword records:  asset 0x1000 path="..." pak="world"
//                     pak file="..." state=verified
//                     generator generation=3 timestamp=...
//   continuations:      mtime_s=... size=... hash=fnv1a64:...
// (a continuation attaches to the last record). Values are quoted iff
// they can carry spaces; the escapes are the CppStr pair (\" and \\).
struct MetaToken {
	bool quoted = false;    // the value carried quotes (needs unescape)
	std::string key;        // "" for a bare word (keyword / hex id)
	std::string value;
};

// Tokenize one metadata payload line (the text after "// "). Returns
// false on a malformed quote (the LINE is skipped, not the parse).
bool TokenizeMetaLine(const std::string& line,
	std::vector<MetaToken>& out) {
	out.clear();
	size_t i = 0;
	const size_t n = line.size();
	while (i < n) {
		while (i < n && line[i] == ' ')
			++i;
		if (i >= n)
			break;
		MetaToken tok;
		if (line[i] == '"') { // a bare quoted word (unused today, kept
			++i;             // for forward formats)
			tok.quoted = true;
			while (i < n && line[i] != '"') {
				if (line[i] == '\\' && i + 1 < n)
					++i;
				tok.value += line[i++];
			}
			if (i >= n)
				return false; // unterminated
			++i;
		}
		else {
			const size_t start = i;
			while (i < n && line[i] != ' ')
				++i;
			tok.value = line.substr(start, i - start);
		}
		const size_t eq = tok.value.find('=');
		if (eq != std::string::npos && !tok.quoted) {
			tok.key = tok.value.substr(0, eq);
			tok.value = tok.value.substr(eq + 1);
			if (!tok.value.empty() && tok.value.front() == '"') {
				if (tok.value.back() != '"'
						|| tok.value.size() < 2)
					return false;
				tok.quoted = true;
				tok.value = tok.value.substr(1,
					tok.value.size() - 2);
				std::string unescaped;
				for (size_t k = 0; k < tok.value.size(); ++k)
					if (tok.value[k] == '\\'
							&& k + 1 < tok.value.size())
						unescaped += tok.value[++k];
					else
						unescaped += tok.value[k];
				tok.value = unescaped;
			}
		}
		out.push_back(std::move(tok));
	}
	return true;
}

// Strip one comment line down to its payload ("" when the line is not
// a metadata comment).
std::string MetaPayload(const std::string& raw) {
	std::string_view v(raw);
	while (!v.empty() && (v.back() == '\r' || v.back() == '\n'))
		v.remove_suffix(1);
	if (v.size() >= 3 && v.substr(0, 3) == "// ")
		return std::string(v.substr(3));
	if (v.size() >= 2 && v.substr(0, 2) == "//")
		return std::string(v.substr(2));
	return std::string();
}

// Parse the EOF metadata out of a header's text. Tolerant: unknown
// keys and malformed lines are SKIPPED (the format may grow); an
// unknown VERSION or a missing section leaves out.present=false (the
// caller scans fresh — never an unsafe cache reuse).
void ParseHeaderMetadataText(const std::string& text,
	HeaderMetadata& out) {
	out = HeaderMetadata{};
	bool inSection = false;
	int version = -1;
	// The last record each section appends continuations to.
	AssetFingerprint* lastAsset = nullptr;
	PakFingerprint* lastPak = nullptr;
	std::istringstream in(text);
	std::string raw;
	while (std::getline(in, raw)) {
		const std::string line = MetaPayload(raw);
		if (line.empty())
			continue;
		if (!inSection) {
			if (line.compare(0, std::strlen(kMetadataBegin),
					kMetadataBegin) == 0) {
				version = std::atoi(
					line.c_str() + std::strlen(kMetadataBegin));
				inSection = version == kMetadataVersion;
			}
			continue;
		}
		if (line.compare(0, std::strlen(kMetadataEnd), kMetadataEnd)
				== 0)
			break; // a well-formed end; keep what we have
		std::vector<MetaToken> toks;
		if (!TokenizeMetaLine(line, toks))
			continue;
		if (toks.empty())
			continue;
		const auto valueOf = [&](const char* key) -> std::string {
			for (const MetaToken& t : toks)
				if (t.key == key)
					return t.value;
			return std::string();
		};
		if (toks[0].key.empty()) { // a keyword record line
			const std::string& kw = toks[0].value;
			if (kw == "[assets]" || kw == "[paks]") {
				lastAsset = nullptr;
				lastPak = nullptr;
				continue;
			}
			if (kw == "generator") {
				out.generation = std::strtoull(
					valueOf("generation").c_str(), nullptr, 10);
				out.timestamp = std::strtoull(
					valueOf("timestamp").c_str(), nullptr, 10);
				continue;
			}
			if (kw == "catalog") {
				out.contentHash = valueOf("hash");
				continue;
			}
			if (kw == "asset" && toks.size() >= 2
					&& toks[1].key.empty()) {
				AssetFingerprint f;
				f.id = std::strtoull(toks[1].value.c_str(),
					nullptr, 16);
				f.path = valueOf("path");
				f.pak = valueOf("pak");
				lastPak = nullptr;
				lastAsset = &out.assets[f.id];
				*lastAsset = std::move(f);
				continue;
			}
			if (kw == "pak") {
				PakFingerprint f;
				f.file = valueOf("file");
				lastAsset = nullptr;
				lastPak = &out.paks[f.file];
				*lastPak = std::move(f);
				continue;
			}
			continue; // an unknown keyword: skip, keep reading
		}
		// A continuation: key=value pairs onto the last record.
		for (const MetaToken& t : toks) {
			if (t.key.empty())
				continue;
			if (lastAsset) {
				if (t.key == "mtime_s")
					lastAsset->mtime = std::strtoll(
						t.value.c_str(), nullptr, 10);
				else if (t.key == "size")
					lastAsset->size = std::strtoull(
						t.value.c_str(), nullptr, 10);
				else if (t.key == "hash")
					lastAsset->hash = t.value;
			}
			else if (lastPak) {
				if (t.key == "mtime_s")
					lastPak->mtime = std::strtoll(
						t.value.c_str(), nullptr, 10);
				else if (t.key == "size")
					lastPak->size = std::strtoull(
						t.value.c_str(), nullptr, 10);
				else if (t.key == "hash")
					lastPak->hash = t.value;
				else if (t.key == "state")
					lastPak->state = t.value;
			}
		}
	}
	out.present = inSection && version == kMetadataVersion;
}

// THE ONE identity-acquisition point (every catalog entity goes through
// here, in this order):
//   1. a VALID sidecar GUID wins — and the row is ADOPTED into the
//      asset directory (the `assets` table: GUID -> path/kind), so the
//      source-asset registry and the content-id registry NEVER
//      disagree about an entity (one database, both tables, one
//      acquisition path);
//   2. a MISSING sidecar ("no .meta") mints a fresh GUID + writes it
//      (the sidecar travels with the file — renames keep the identity)
//      + adopts;
//   3. a MALFORMED/UNREADABLE sidecar FAILS THE GENERATION WITHOUT
//      TOUCHING IT. Auto-re-minting would silently REPLACE a stable
//      identity (every content id keyed on the old guid orphaned) —
//      identity repair is a SEPARATE, EXPLICIT operation: the error
//      tells the author to delete or fix the sidecar by hand (a
//      deletion re-enters the missing path on the next run).
// `sidecarOut` (optional) receives the sidecar path (the depfile feed —
// identity inputs belong with the content deps).
bool AcquireAssetIdentity(smvk::cache::AssetDatabase& db,
	const std::string& absPath, const std::string& kind,
	std::string& outGuid, std::string& sidecarOut, std::string& note,
	std::string& err) {
	outGuid.clear();
	note.clear();
	sidecarOut = smvk::cache::MetaPathFor(absPath);
	std::string guid;
	std::string metaErr;
	if (smvk::cache::ReadAssetMeta(absPath, guid, metaErr)) {
		if (!db.AdoptFile(absPath, guid, kind, err))
			return false;
		outGuid = guid;
		return true;
	}
	if (metaErr != "no .meta") {
		// Malformed/unreadable — never auto-repair a stable identity.
		err = "the identity sidecar '" + sidecarOut + "' is unreadable"
			" (" + metaErr + ") — REFUSING to auto-replace it (every"
				" content id keyed on that guid would silently orphan)."
				" Identity repair is explicit: fix or delete the sidecar"
				" by hand (a deleted sidecar re-mints on the next run)";
		return false;
	}
	outGuid = smvk::tour::GenerateUuid();
	if (!smvk::cache::WriteAssetMeta(absPath, outGuid, "content-id",
			err))
		return false;
	if (!db.AdoptFile(absPath, outGuid, kind, err))
		return false;
	note = "sidecar was missing — identity MINTED at '"
		+ sidecarOut + "'";
	return true;
}

} // namespace

bool GenerateProjectCatalog(const Project& project, const BuildPlan& plan,
	smvk::cache::AssetDatabase& db, ProjectCatalog& out,
	std::vector<std::string>& contentDeps, std::string& err) {
	err.clear();
	out = ProjectCatalog{};
	contentDeps.clear();
	if (!project.Valid()) {
		err = "invalid project (open/init first)";
		return false;
	}
	if (plan.paks.empty()) {
		err = "the plan is empty (resolve a recipe first)";
		return false;
	}
	out.projectName = plan.projectName.empty()
		? project.name : plan.projectName;
	// The stable id comes from the PERSISTENT identity (the marker's
	// guid — see project.h), never the name or the path: a rename or
	// a root move keeps every compiled-in project::ProjectId valid.
	if (!smvk::cache::IsValidAssetGuid(project.guid)) {
		err = "the project carries no valid identity (guid) — re-init"
			" the project ('Project.exe init <dir>') or recreate it";
		return false;
	}
	out.projectId = DeriveProjectId(project.guid);
	for (const PlanPak& pak : plan.paks)
		out.paks.emplace_back(pak.id, pak.output);

	// Every ensured (guid -> where it landed) for the tombstone sweep.
	std::set<std::string> liveGuids;
	const auto ensureAsset =
		[&](const std::string& projectRel, const std::string& kind,
			const std::string& pakId, const std::string& guidOverride,
			std::vector<CatalogEntry>& bucket) -> bool {
		const fs::path abs = fs::path(project.root) / fs::path(projectRel);
		std::string guid;
		if (!guidOverride.empty()) {
			// The identity was resolved elsewhere (the mesh's DERIVED
			// manifest GUID): ADOPT it into the asset directory so both
			// tables agree, then ensure the content id.
			guid = guidOverride;
			std::string aErr;
			if (!db.AdoptFile(abs.lexically_normal().generic_string(),
					guid, kind, aErr)) {
				err = "adopt failed for '" + projectRel + "': " + aErr;
				return false;
			}
		}
		else {
			std::string sidecar, note;
			if (!AcquireAssetIdentity(db,
					abs.lexically_normal().generic_string(), kind, guid,
					sidecar, note, err))
				return false;
			if (!note.empty())
				std::printf("[ids] %s\n", note.c_str());
			// Identity inputs belong with the content deps: a sidecar
			// GUID change regenerates (the depfile feed).
			contentDeps.push_back(sidecar);
		}
		uint64_t id = 0;
		std::string symbol;
		if (!db.EnsureContentId(guid, kind, projectRel, id, symbol, err))
			return false;
		liveGuids.insert(guid);
		bucket.push_back(
			CatalogEntry{ id, symbol, projectRel, pakId, kind });
		contentDeps.push_back(abs.lexically_normal().generic_string());
		return true;
	};

	// Scenes: every scene pak's tour (meshes' families below).
	std::map<std::string, std::string> meshGuidByPath;
	for (const PlanPak& pak : plan.paks) {
		if (pak.scene.empty())
			continue;
		if (!ensureAsset(pak.scene, "scene", pak.id, {}, out.scenes))
			return false;
		if (pak.id == plan.entryPak)
			out.entryScene = out.scenes.back().id;
		// The scene's geometry families: the manifest's DERIVED
		// .geometry entry GUID is the MESH's canonical identity — the
		// SAME GUID the asset directory registers the derived blob
		// under, so the runtime content id, the source-asset registry
		// and the import manifest agree on ONE identity (the source
		// glTF's own GUID stays the SOURCE model's identity; keying the
		// mesh row on it was the identity split this fixes).
		for (const std::string& artifact : pak.sceneArtifacts) {
			const std::string ext =
				fs::path(artifact).extension().generic_string();
			std::string familySource;
			if (ext == ".gltf" || ext == ".glb")
				familySource = artifact;
			else {
				fs::path src = fs::path(artifact);
				for (const char* e : { ".gltf", ".glb" }) {
					src.replace_extension(e);
					if (fs::is_regular_file(
							fs::path(project.root) / src))
						break;
					src = fs::path(artifact);
				}
				familySource = src.generic_string();
			}
			smvk::assets::AssetManifest man;
			std::string mErr;
			if (smvk::assets::LoadAssetManifest(
					(fs::path(project.root) / fs::path(familySource))
						.lexically_normal().generic_string(),
					man, mErr)) {
				const std::string want =
					fs::path(artifact).filename().generic_string();
				std::string derived;
				for (const auto& e : man.entries)
					if (e.file == want) {
						derived = e.guid;
						break;
					}
				if (derived.empty())
					for (const auto& e : man.entries)
						if (e.kind == smvk::assets::AssetKind::Geometry) {
							derived = e.guid;
							break;
						}
				if (!derived.empty())
					meshGuidByPath[artifact] = derived;
				// The manifest IS an identity input — a dep.
				contentDeps.push_back(
					smvk::cache::MetaPathFor(
						(fs::path(project.root)
							/ fs::path(familySource))
							.lexically_normal().generic_string()));
			}
			// (No manifest: the mesh pass below mints its own sidecar
			// identity through AcquireAssetIdentity.)
		}
	}
	// Owned assets: meshes (geometry) + textures + sounds; BakeFiles
	// are bake plumbing (no game-loadable identity in v1 — documented).
	for (const PlanPak& pak : plan.paks) {
		for (const PlanAsset& asset : pak.assets) {
			if (asset.kind == PlanAssetKind::Geometry) {
				if (!ensureAsset(asset.path, "mesh", pak.id,
						meshGuidByPath.count(asset.path)
							? meshGuidByPath[asset.path] : std::string(),
						out.meshes))
					return false;
			}
			else if (asset.kind == PlanAssetKind::Texture) {
				if (!ensureAsset(asset.path, "texture", pak.id, {},
						out.textures))
					return false;
			}
			else if (asset.kind == PlanAssetKind::Sound) {
				// Sounds: the .wav's own .meta sidecar identity (the
				// texture form — AcquireAssetIdentity mints it when
				// absent), ids in the 0x4xxx range.
				if (!ensureAsset(asset.path, "sound", pak.id, {},
						out.sounds))
					return false;
			}
			else if (asset.kind == PlanAssetKind::Shader) {
				// PROJECT-AUTHORED shaders: the .spv's own sidecar
				// identity, ids in the 0x5xxx range. The ENGINE-NATIVE
				// set never flows through a plan (engine-shaders.pak
				// is build-owned — recipes never control it).
				if (!ensureAsset(asset.path, "shader", pak.id, {},
						out.shaders))
					return false;
			}
		}
	}

	// The tombstone sweep: active registry rows whose guid the plan no
	// longer carries drop to active=0 (id+symbol reserved forever).
	{
		std::vector<smvk::cache::ContentIdRecord> rows;
		std::string lErr;
		if (!db.ListContentIds(rows, lErr)) {
			err = "registry list failed: " + lErr;
			return false;
		}
		// The RETIRED table (the tombstone contract): the pre-existing
		// tombstones + the rows this pass tombstones — every inactive
		// row, none of the re-activated ones (an inactive row whose
		// guid the plan carries was re-activated by EnsureContentId
		// above). The header emits the historical typed symbols
		// (values preserved) + the compile-time IsRetired/IsActive
		// queries; retired entries stay out of the active namespaces
		// and the manifests.
		const auto retireRow = [&](const smvk::cache::ContentIdRecord&
				row) {
			CatalogEntry e;
			e.id = row.id;
			e.symbol = row.symbol;
			e.path = row.path;
			e.kind = row.kind;
			out.retired.push_back(std::move(e));
		};
		for (const auto& row : rows) {
			if (!row.active && !liveGuids.count(row.guid))
				retireRow(row); // a tombstone from an earlier run
		}
		for (const auto& row : rows) {
			if (!row.active || liveGuids.count(row.guid))
				continue;
			std::string sErr;
			if (!db.SetContentIdActive(row.guid, false, sErr)) {
				err = "tombstone failed: " + sErr;
				return false;
			}
			retireRow(row); // tombstoned by THIS generation
		}
		std::sort(out.retired.begin(), out.retired.end(),
			[](const CatalogEntry& a, const CatalogEntry& b) {
				return a.id < b.id;
			});
	}

	// Deterministic order: id-sorted buckets (EnsureContentId allocated
	// in plan order; the sort makes the emission stable against plan
	// re-orderings).
	const auto byId = [](const CatalogEntry& a, const CatalogEntry& b) {
		return a.id < b.id;
	};
	std::sort(out.scenes.begin(), out.scenes.end(), byId);
	std::sort(out.textures.begin(), out.textures.end(), byId);
	std::sort(out.meshes.begin(), out.meshes.end(), byId);
	std::sort(out.sounds.begin(), out.sounds.end(), byId);
	std::sort(out.shaders.begin(), out.shaders.end(), byId);
	return true;
}

// The catalog IDENTITY fingerprint (the shared fold, appmanifest.h's
// ComputeCatalogFingerprint): over the entries in EMISSION order
// (bucket order: scenes, textures, meshes, sounds; id-sorted within -
// exactly what the header's Assets array and the manifest's [ids]
// rows both enumerate), folding (id, kind byte, pakIndex, path).
// The SAME implementation the manifest writer uses - one fold, no
// drift between the compiled and deployed halves.
uint64_t CatalogFingerprintOf(const ProjectCatalog& c) {
	std::vector<uint64_t> ids;
	std::vector<uint8_t> kinds;
	std::vector<uint32_t> pakIndices;
	std::vector<const char*> paths;
	const auto push = [&](const std::vector<CatalogEntry>& entries,
			uint8_t kind) {
		for (const CatalogEntry& e : entries) {
			uint32_t pakIndex = 0;
			for (size_t pi = 0; pi < c.paks.size(); ++pi)
				if (c.paks[pi].first == e.pak) {
					pakIndex = uint32_t(pi);
					break;
				}
			ids.push_back(e.id);
			kinds.push_back(kind);
			pakIndices.push_back(pakIndex);
			paths.push_back(e.path.c_str());
		}
	};
	push(c.scenes, smvk::kCatalogKindScene);
	push(c.textures, smvk::kCatalogKindTexture);
	push(c.meshes, smvk::kCatalogKindMesh);
	push(c.sounds, smvk::kCatalogKindSound);
	push(c.shaders, smvk::kCatalogKindShader);
	return smvk::ComputeCatalogFingerprint(ids.data(), kinds.data(),
		pakIndices.data(), paths.data(), ids.size());
}

std::string EmitProjectHeader(const ProjectCatalog& c,
	const GenerationStamp& stamp,
	const std::vector<AssetFingerprint>& assets,
	const std::vector<PakFingerprint>& paks) {
	std::ostringstream os;
	os << "// Generated by Tourmaline (Project.exe ids). Do not edit.\n"
		<< "// The content-as-code catalog: stable ids from the project's\n"
		<< "// asset database (assets.db - the one identity store); the\n"
		<< "// same content ships in the paks below. GENERATION-STAMPED\n"
		<< "// (the version block below): the stamp moves only on a\n"
		<< "// generation event; an unchanged generation re-emits this\n"
		<< "// file byte-identical (write-only-on-change).\n"
		<< "#pragma once\n\n"
		<< "#include <array>\n#include <cstdint>\n"
			"#include <string_view>\n\n"
		<< "namespace project {\n\n"
		<< "using Id = std::uint64_t;\n\n"

		<< "// TYPED KEYS (which ASSET - stable catalog identities; the\n"
		<< "// RUNTIME INSTANCE handles World::SpawnMesh & co. return\n"
		<< "// are a different thing entirely - Engine/Scene/entity_handle.h).\n"
		<< "// Implicit Id() conversion keeps the raw-id call sites\n"
		<< "// working; the types make cross-kind misuse a compile error.\n"
		<< "struct SceneKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "struct TextureKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "struct MeshKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "struct SoundKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "// PROJECT-AUTHORED shader assets (the 0x5xxx range -"
		<< " compiled .spv the project OWNS, distinct from the"
		<< " ENGINE-NATIVE set engine-shaders.pak carries).\n"
		<< "struct ShaderKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "// (SkeletonKey / AnimationClipKey arrive with their catalog\n"
		<< "// buckets - the .anim asset identity kinds; the HANDLE types\n"
		<< "// they pair with are already in entity_handle.h.)\n"
		<< "struct SkeletonKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "struct AnimationClipKey { Id id = 0;"
		<< " constexpr operator Id() const { return id; } };\n\n"
		<< "inline constexpr Id ProjectId = 0x" << std::hex
		<< c.projectId << std::dec << "ULL;\n"
		<< "inline constexpr std::string_view Name = \""
		<< CppStr(c.projectName) << "\";\n\n"

		<< "// The generation stamp (cheap version control for asset\n"
		<< "// time): the timestamp + generation move ONLY on a\n"
		<< "// generation event - a catalog/content change, the explicit\n"
		<< "// <Name>_stamp refresh, or a recipe build that moved the\n"
		<< "// fingerprints. GeneratedAt is unix seconds. ContentHash is\n"
		<< "// the catalog SEMANTICS fingerprint (ids, symbols, paths,\n"
		<< "// paks, entry scene) - a texture repixel keeps it; the EOF\n"
		<< "// metadata at the bottom of this file carries the new file\n"
		<< "// hash instead.\n"
		<< "inline constexpr std::uint64_t Generation = "
		<< stamp.generation << "ULL;\n"
		<< "inline constexpr std::uint64_t GeneratedAt = "
		<< stamp.timestamp << "ULL;\n"
		<< "inline constexpr std::string_view ContentHash = \""
		<< CppStr(stamp.contentHash) << "\";\n\n";

	const auto emitNamespace = [&](const char* name,
			const std::vector<CatalogEntry>& entries,
			const char* keyType) {
		// The typed-key constants: each namespace's entries become
		// typed keys (SceneKey/TextureKey/MeshKey/SoundKey) - game
		// code writes `MeshKey mesh = project::meshes::Knight;` and
		// the zero value reads as "no key".
		os << "namespace " << name << " {\n";
		for (const CatalogEntry& e : entries)
			os << "    inline constexpr " << keyType << " "
				<< e.symbol << "{0x"
				<< std::hex << e.id << std::dec << "ULL};\n";
		os << "}\n\n";
	};
	emitNamespace("scenes", c.scenes, "SceneKey");
	emitNamespace("textures", c.textures, "TextureKey");
	emitNamespace("meshes", c.meshes, "MeshKey");
	emitNamespace("sounds", c.sounds, "SoundKey");
	emitNamespace("shaders", c.shaders, "ShaderKey");
	os << "// Runtime packages (plan/mount order).\n"
		<< "struct Pak {\n"
		<< "    std::string_view name;\n"
		<< "    std::string_view file;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Paks = {\n";
	for (const auto& [id, file] : c.paks)
		os << "    Pak{\"" << CppStr(id) << "\", \""
			<< CppStr(file) << "\"},\n";
	os << "};\n\n"
		<< "// The asset KIND enum - the values are an ABI (the runtime's\n"
		<< "// catalog-fingerprint fold + the .tapp validation agree on\n"
		<< "// these bytes; do not renumber).\n"
		<< "enum class AssetKind : std::uint8_t {\n"
		<< "    Scene = 0,\n"
		<< "    Texture = 1,\n"
		<< "    Mesh = 2,\n"
		<< "    Sound = 3,\n"
		<< "    Shader = 4,\n"
		<< "};\n\n"
		<< "// The FLAT TYPED CATALOG: plain compile-time POD records -\n"
		<< "// no startup strings, no maps, no runtime discovery table.\n"
		<< "// pakIndex indexes Paks (the LOGICAL package; the published\n"
		<< "// content-addressed filename is the DEPLOYMENT manifest's\n"
		<< "// job - the .tapp maps it). sceneName: the cooked-scene\n"
		<< "// identity is cook-state, not stable identity, so it stays\n"
		<< "// deployment-side (the .tapp's scene rows) - the field is\n"
		<< "// reserved and emitted empty.\n"
		<< "struct AssetDesc {\n"
		<< "    Id id;\n"
		<< "    AssetKind kind;\n"
		<< "    std::uint32_t pakIndex;\n"
		<< "    std::string_view path;\n"
		<< "    std::string_view sceneName;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Assets = {\n";
	const auto emitRows = [&](
			const std::vector<CatalogEntry>& entries, const char* ns,
			const char* kind) {
		for (const CatalogEntry& e : entries) {
			// The entry's pak is the LOGICAL pak id; its index into
			// c.paks (plan order) is the compiled reference.
			uint32_t pakIndex = 0;
			for (size_t pi = 0; pi < c.paks.size(); ++pi)
				if (c.paks[pi].first == e.pak) {
					pakIndex = uint32_t(pi);
					break;
				}
			os << "    AssetDesc{" << ns << "::" << e.symbol
				<< ", AssetKind::" << kind << ", " << pakIndex
				<< ", \"" << CppStr(e.path) << "\", \"\"},\n";
		}
	};
	emitRows(c.scenes, "scenes", "Scene");
	emitRows(c.textures, "textures", "Texture");
	emitRows(c.meshes, "meshes", "Mesh");
	emitRows(c.sounds, "sounds", "Sound");
	emitRows(c.shaders, "shaders", "Shader");
	os << "};\n\n"
		<< "// The catalog identity fingerprint: FNV-1a64 over the rows'\n"
		<< "// stable identity in emission order (appmanifest.h's\n"
		<< "// ComputeCatalogFingerprint - the exact fold). The .tapp\n"
		<< "// deployment manifest carries the same fold over its [ids]\n"
		<< "// rows; ProjectRuntime::AttachCompiledCatalog compares them,\n"
		<< "// so a compiled game and its deployed content can never\n"
		<< "// silently disagree.\n"
		<< "inline constexpr std::uint64_t CatalogFingerprint = 0x"
		<< std::hex << CatalogFingerprintOf(c) << std::dec
		<< "ULL;\n\n"
		<< "// Compile-time lookup over the flat table (a constexpr\n"
		<< "// linear walk - the tables are tiny and the query is\n"
		<< "// compile-time; no runtime map, no startup strings).\n"
		<< "constexpr const AssetDesc* Find(Id id) {\n"
		<< "    for (const AssetDesc& asset : Assets)\n"
		<< "        if (asset.id == id)\n"
		<< "            return &asset;\n"
		<< "    return nullptr;\n"
		<< "}\n\n";
	if (c.entryScene) {
		// The entry scene's symbol (the catalog's scenes bucket is
		// id-sorted; find it back).
		std::string entrySymbol;
		for (const CatalogEntry& e : c.scenes)
			if (e.id == c.entryScene) {
				entrySymbol = e.symbol;
				break;
			}
		os << "inline constexpr Id EntryScene = scenes::" << entrySymbol
			<< ";\n\n";
	}
	os << "// Compile-time content validation.\n"
		<< "constexpr bool Contains(Id id) {\n"
		<< "    for (const AssetDesc& asset : Assets)\n"
		<< "        if (asset.id == id)\n"
		<< "            return true;\n"
		<< "    return false;\n"
		<< "}\n\n";

	// The RETIRED identities (the tombstone contract): deleted assets
	// whose ids+symbols stay reserved FOREVER (never reused). The
	// historical TYPED SYMBOLS keep their numeric values for stable
	// API compatibility; the entries are excluded from the active
	// namespaces, the Assets array and the published manifests.
	// IsRetired/IsActive are the COMPILE-TIME active-membership
	// queries. Re-adding the asset with its original GUID re-activates
	// the SAME id+symbol. (Emitted AFTER Contains - the queries
	// reference it.)
	if (c.retired.empty()) {
		os << "// No retired assets (the tombstone table is empty).\n"
			<< "inline constexpr std::array<Id, 0> RetiredIds = {};\n"
			<< "constexpr bool IsRetired(Id) { return false; }\n"
			<< "constexpr bool IsActive(Id id) { return Contains(id); }\n"
			<< "\n";
	}
	else {
		const auto keyTypeOf = [](const std::string& kind) {
			if (kind == "scene")
				return "SceneKey";
			if (kind == "texture")
				return "TextureKey";
			if (kind == "mesh")
				return "MeshKey";
			if (kind == "sound")
				return "SoundKey";
			if (kind == "shader")
				return "ShaderKey";
			return "Id"; // an unknown kind degrades to the raw id
		};
		os << "// RETIRED assets (tombstones): the historical identities,\n"
			<< "// typed keys with their values PRESERVED (stable API\n"
			<< "// compatibility); never in the active namespaces, never\n"
			<< "// reused. A runtime load of a retired id fails (the\n"
			<< "// compiled catalog carries no row for it).\n"
			<< "namespace retired {\n";
		for (const CatalogEntry& e : c.retired) {
			const std::string kt = keyTypeOf(e.kind);
			if (kt == "Id")
				os << "    inline constexpr Id " << e.symbol
					<< "{0x" << std::hex << e.id << std::dec
					<< "ULL};\n";
			else
				os << "    inline constexpr " << kt << " "
					<< e.symbol << "{0x" << std::hex << e.id
					<< std::dec << "ULL};\n";
		}
		os << "}\n\n"
			<< "inline constexpr std::array RetiredIds = {\n";
		for (const CatalogEntry& e : c.retired)
			os << "    0x" << std::hex << e.id << std::dec
				<< "ULL,\n";
		os << "};\n"
			<< "// The compile-time active-membership queries: retired\n"
			<< "// ids are OUT of the active catalog even if a stale\n"
			<< "// compiled row ever names one.\n"
			<< "constexpr bool IsRetired(Id id) {\n"
			<< "    for (Id r : RetiredIds)\n"
			<< "        if (r == id) return true;\n"
			<< "    return false;\n"
			<< "}\n"
			<< "constexpr bool IsActive(Id id) {\n"
			<< "    return Contains(id) && !IsRetired(id);\n"
			<< "}\n"
			<< "\n";
	}
	os << "} // namespace project\n\n";

	// ---- the EOF metadata (the tools' half of the file) --------------
	// A machine-readable COMMENT: the compiler never sees it, nothing
	// codes against it. It carries the generation stamp + the last
	// generation's fingerprints (the generator's incremental cache —
	// no side file) + the paks' cook-state claims. Quoted values use
	// the CppStr escape pair; records are keyed by stable id / pak
	// file name; hash algorithms and time units are explicit.
	const auto q = [](const std::string& s) {
		return "\"" + CppStr(s) + "\"";
	};
	os << "// =====================================================================\n"
		<< "// " << kMetadataBegin << " " << kMetadataVersion << "\n"
		<< "//\n"
		<< "// Machine-readable build metadata - a COMMENT, never coded\n"
		<< "// against (the compiler ignores it; the Tourmaline tools\n"
		<< "// read it). The generator's incremental cache: the last\n"
		<< "// generation's fingerprints live here, so a refresh rehashes\n"
		<< "// only what mtime+size say moved. An unknown version reads\n"
		<< "// as absent (a fresh scan, never a stale-cache reuse).\n"
		<< "//\n"
		<< "// generator generation=" << stamp.generation
		<< " timestamp=" << stamp.timestamp << "\n"
		<< "// catalog hash=" << stamp.contentHash << "\n"
		<< "//\n"
		<< "// [assets]\n";
	for (const AssetFingerprint& f : assets) {
		os << "// asset 0x" << std::hex << f.id << std::dec
			<< " path=" << q(f.path) << " pak=" << q(f.pak)
			<< "\n"
			<< "//   mtime_s=" << f.mtime << " size=" << f.size;
		if (!f.hash.empty())
			os << " hash=" << f.hash;
		os << "\n";
	}
	os << "//\n"
		<< "// [paks]\n";
	for (const PakFingerprint& f : paks) {
		os << "// pak file=" << q(f.file) << "\n"
			<< "//   mtime_s=" << f.mtime << " size=" << f.size;
		if (!f.hash.empty())
			os << " hash=" << f.hash;
		if (!f.state.empty())
			os << " state=" << f.state;
		os << "\n";
	}
	os << "//\n"
		<< "// " << kMetadataEnd << " " << kMetadataVersion << "\n"
		<< "// =====================================================================\n";
	return os.str();
}

std::string ComputeCatalogContentHash(const ProjectCatalog& c) {
	// The CANONICAL form — exactly what "the catalog" means
	// semantically, never the emitted header text (that carries the
	// moving stamp + the fingerprints). Id-sorted buckets keep the
	// hash stable against plan re-orderings (the emission sorts for
	// the same reason); the kind tags + field separators make the
	// serialization unambiguous.
	std::string canonical;
	canonical += "tourmaline-catalog-v1\n";
	canonical += "name " + c.projectName + "\n";
	canonical += "project 0x" + Hex64(c.projectId) + "\n";
	canonical += "entry 0x" + Hex64(c.entryScene) + "\n";
	for (const auto& [id, file] : c.paks)
		canonical += "pak " + id + " " + file + "\n";
	const auto addBucket = [&](const char* kind,
			const std::vector<CatalogEntry>& entries) {
		std::vector<CatalogEntry> sorted(entries);
		std::sort(sorted.begin(), sorted.end(),
			[](const CatalogEntry& a, const CatalogEntry& b) {
				return a.id < b.id;
			});
		canonical += std::string("kind ") + kind + "\n";
		for (const CatalogEntry& e : sorted)
			canonical += "asset 0x" + Hex64(e.id) + " " + e.symbol
				+ " " + e.path + " " + e.pak + "\n";
	};
	addBucket("scene", c.scenes);
	addBucket("texture", c.textures);
	addBucket("mesh", c.meshes);
	addBucket("sound", c.sounds);
	addBucket("shader", c.shaders);
	// The retired set is catalog SEMANTICS too: a retire/reactivate
	// moves the content hash (a generation event), so the compiled
	// catalog and its hash can never disagree about membership.
	canonical += "retired\n";
	for (const CatalogEntry& e : c.retired)
		canonical += "asset 0x" + Hex64(e.id) + " " + e.symbol + " "
			+ e.kind + "\n";
	return smvk::cache::HashBytes(canonical.data(), canonical.size());
}

bool ReadProjectHeaderMetadata(const std::string& path,
	HeaderMetadata& out, std::string& err) {
	err.clear();
	bool exists = false;
	std::string text;
	if (!ReadTextFile(path, exists, text)) {
		err = "the project header exists but cannot be read: " + path;
		return false;
	}
	ParseHeaderMetadataText(text, out);
	return true;
}

bool StatFile(const std::string& absPath, int64_t& mtime,
	uint64_t& size, std::string& err) {
	err.clear();
	mtime = -1;
	size = 0;
	std::error_code ec;
	const fs::path p(absPath);
	if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec))
		return true; // missing reads as -1/0 — never an error
	const fs::file_time_type lwt = fs::last_write_time(p, ec);
	if (ec) {
		err = "stat failed for '" + absPath + "': " + ec.message();
		return false;
	}
	const uintmax_t bytes = fs::file_size(p, ec);
	if (ec) {
		err = "size failed for '" + absPath + "': " + ec.message();
		return false;
	}
	mtime = ToUnixSeconds(lwt);
	size = uint64_t(bytes);
	return true;
}

bool FingerprintFile(const std::string& absPath,
	int64_t& mtime, uint64_t& size, std::string& hash,
	std::string& err) {
	err.clear();
	hash.clear();
	mtime = -1;
	size = 0;
	if (!StatFile(absPath, mtime, size, err))
		return false;
	if (mtime < 0)
		return true; // missing: -1/0/""
	// ALWAYS hash from bytes: a matching size + mtime is NEVER proof
	// of unchanged content (preserved-timestamp and backdated
	// same-size edits are exactly the lie this refuses). The mtime/
	// size ride the EOF records as diagnostics + the demotion-only
	// cook-state hints — not as freshness.
	std::ifstream in(absPath, std::ios::binary);
	if (!in) {
		err = "cannot open for hashing: " + absPath;
		return false;
	}
	std::string fileBytes((std::istreambuf_iterator<char>(in)),
		std::istreambuf_iterator<char>());
	if (in.bad()) {
		err = "failed reading for hashing: " + absPath;
		return false;
	}
	hash = "fnv1a64:" + smvk::cache::HashBytes(fileBytes.data(),
		fileBytes.size());
	return true;
}

bool CollectAssetFingerprints(const Project& project,
	const ProjectCatalog& catalog,
	std::vector<AssetFingerprint>& out, std::string& err) {
	err.clear();
	out.clear();
	const auto add = [&](const std::vector<CatalogEntry>& entries) {
		for (const CatalogEntry& e : entries) {
			AssetFingerprint f;
			f.id = e.id;
			f.path = e.path;
			f.pak = e.pak;
			const fs::path abs = (fs::path(project.root)
				/ fs::path(e.path)).lexically_normal();
			std::string fErr;
			if (!FingerprintFile(abs.generic_string(),
					f.mtime, f.size, f.hash, fErr)) {
				err = "fingerprint failed for '" + e.path + "': "
					+ fErr;
				return false;
			}
			out.push_back(std::move(f));
		}
		return true;
	};
	return add(catalog.scenes) && add(catalog.textures)
		&& add(catalog.meshes) && add(catalog.sounds)
		&& add(catalog.shaders);
}

bool DiscoverPakFingerprints(const std::string& pakDir,
	const ProjectCatalog& catalog,
	const std::vector<AssetFingerprint>& assets,
	const HeaderMetadata& prev, std::vector<PakFingerprint>& out,
	std::string& err) {
	err.clear();
	out.clear();
	// 1) the LIVE pak set: every published .tapp in the dir names its
	//    paks, and the [ids] rows join STABLE asset ids to pak FILES —
	//    never file-name guessing (the content-addressed names carry
	//    digests the catalog pass cannot predict). A manifest that
	//    fails to load names nothing (not ours / corrupt).
	std::map<uint64_t, std::string> pakFileByAssetId;
	{
		std::error_code ec;
		const fs::path dir(pakDir);
		if (fs::exists(dir, ec)) {
			for (const fs::directory_entry& e :
					fs::directory_iterator(dir, ec)) {
				std::error_code fileEc;
				if (!e.is_regular_file(fileEc))
					continue;
				if (e.path().extension().generic_string() != ".tapp")
					continue;
				smvk::AppManifest manifest;
				std::string mErr;
				if (!smvk::LoadAppManifest(
						e.path().generic_string(), manifest, mErr))
					continue;
				for (const smvk::AppManifestAsset& row
						: manifest.assets)
					pakFileByAssetId[row.id] = row.pak;
			}
		}
	}
	// The catalog's assets grouped by pak id (the staleness witness).
	std::map<std::string, std::vector<const AssetFingerprint*>> byPak;
	for (const AssetFingerprint& f : assets)
		byPak[f.pak].push_back(&f);
	// 2) per catalog pak (plan order — deterministic): resolve the
	//    published file, fingerprint it (ALWAYS from bytes — the pak's
	//    content hash backs the state claim, never an mtime+size
	//    carry), decide the cook state.
	for (const auto& [pakId, outputFile] : catalog.paks) {
		PakFingerprint f;
		for (const AssetFingerprint* a : byPak[pakId]) {
			const auto it = pakFileByAssetId.find(a->id);
			if (it != pakFileByAssetId.end()) {
				f.file = it->second;
				break;
			}
		}
		// Fallback: a pre-content-addressing plain output name.
		if (f.file.empty() && !outputFile.empty()) {
			std::error_code ec;
			if (fs::is_regular_file(
					fs::path(pakDir) / fs::path(outputFile), ec))
				f.file = outputFile;
		}
		if (f.file.empty()) {
			f.state = "missing";
			out.push_back(std::move(f));
			continue;
		}
		std::string fErr;
		if (!FingerprintFile(
				(fs::path(pakDir) / fs::path(f.file)).lexically_normal()
					.generic_string(),
				f.mtime, f.size, f.hash, fErr)) {
			err = "pak fingerprint failed for '" + f.file + "': "
				+ fErr;
			return false;
		}
		if (f.mtime < 0) { // vanished between the join and the stat
			f.state = "missing";
			out.push_back(std::move(f));
			continue;
		}
		// The cook-state rules (a CLAIM, never a content proof — only
		// the recipe build's re-stamp says verified from knowledge):
		//   stale if (a) an asset of the pak is UNPUBLISHED (no
		//          manifest row — the pak cannot contain it),
		//          (b) an asset's fingerprint CHANGED since the last
		//              generation (no previous record counts as
		//              changed: a first-ever run claims nothing), or
		//          (c) a source is NEWER than the pak file.
		bool stale = false;
		for (const AssetFingerprint* a : byPak[pakId]) {
			if (!pakFileByAssetId.count(a->id)) {
				stale = true; // (a)
				break;
			}
			const auto pa = prev.assets.find(a->id);
			if (pa == prev.assets.end()
					|| pa->second.hash != a->hash) {
				stale = true; // (b)
				break;
			}
			if (a->mtime > f.mtime) {
				stale = true; // (c)
				break;
			}
		}
		f.state = stale ? "stale" : "verified";
		out.push_back(std::move(f));
	}
	return true;
}

void ComposeProjectHeader(const ProjectCatalog& c,
	const std::vector<AssetFingerprint>& assets,
	const std::vector<PakFingerprint>& paks, const std::string& path,
	bool forceStamp, std::string& text, GenerationStamp& stamp,
	bool& changed) {
	// The no-op probe FIRST: the CURRENT content with the PREVIOUS
	// stamp — byte-equal to the on-disk file means nothing moved, the
	// generation stays, and the caller's write-only-on-change never
	// touches the file (no recompile of anything including it).
	bool exists = false;
	std::string existing;
	ReadTextFile(path, exists, existing); // (a read failure composes
	                                      // from scratch; the write
	                                      // below surfaces it)
	HeaderMetadata prev;
	ParseHeaderMetadataText(existing, prev);
	const std::string contentHash = ComputeCatalogContentHash(c);
	if (!forceStamp && prev.present) {
		GenerationStamp probe;
		probe.generation = prev.generation;
		probe.timestamp = prev.timestamp;
		probe.contentHash = contentHash;
		text = EmitProjectHeader(c, probe, assets, paks);
		if (text == existing) {
			stamp = probe;
			changed = false;
			return;
		}
	}
	// A GENERATION EVENT: the timestamp is the event time, the
	// generation counts them (a legacy/pre-stamp header restarts at 1).
	stamp.generation = prev.generation + 1;
	stamp.timestamp = uint64_t(std::time(nullptr));
	stamp.contentHash = contentHash;
	text = EmitProjectHeader(c, stamp, assets, paks);
	changed = text != existing;
}

bool WriteProjectHeaderIfChanged(const std::string& path,
	const std::string& text, bool& wrote, std::string& err) {
	wrote = false;
	{
		std::ifstream in(path, std::ios::binary);
		if (in) {
			std::string existing((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			if (existing == text)
				return true; // unchanged — the mtime never moves
		}
	}
	std::ofstream f(path, std::ios::binary | std::ios::trunc);
	if (!f) {
		err = "cannot write the project header: " + path;
		return false;
	}
	f << text;
	if (!f.good()) {
		err = "failed writing the project header: " + path;
		return false;
	}
	wrote = true;
	return true;
}

} // namespace smvk::project
