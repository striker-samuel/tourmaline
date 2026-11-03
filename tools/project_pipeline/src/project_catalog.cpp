// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// The project.h generator (see project_catalog.h). Three passes:
// identity (every plan entity's GUID — sidecar read or mint+adopt),
// registry (EnsureContentId + the tombstone sweep), emission (the
// constexpr header text, byte-deterministic for the same catalog).

#include <project_pipeline/project_catalog.h>

#include <Engine/Asset/asset_plan.h>

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
#include <Engine/Asset/audiograph.h>
#include <Engine/Asset/spriteset.h>
#include <Engine/Asset/state_machine.h>
#include <Engine/Asset/navmesh.h>
#include <Engine/Asset/shape_asset.h>
#include <Engine/Asset/pak.h>
#include <Engine/Asset/pak_pager.h>
#include <Engine/Asset/tour.h>

#include <artifact_cache/artifact_cache.h>

#include <gltf_import/asset_export.h>
#include <gltf_import/source_formats.h>

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

// Emit tabular C++ initializers with vertically aligned columns.
// Every cell includes its trailing punctuation (e.g. the comma), so
// padding comes AFTER punctuation, as in normal hand-formatted C++.
// This affects only whitespace: row order and literal bytes are unchanged.
void EmitAlignedRows(std::ostringstream& os,
	const std::vector<std::vector<std::string>>& rows) {
	if (rows.empty())
		return;
	const size_t columnCount = rows.front().size();
	std::vector<size_t> widths(columnCount, 0);
	for (const auto& row : rows)
		for (size_t i = 0; i < columnCount; ++i)
			widths[i] = std::max(widths[i], row[i].size());
	for (const auto& row : rows) {
		os << "    ";
		for (size_t i = 0; i < columnCount; ++i) {
			os << row[i];
			if (i + 1 < columnCount)
				os << std::string(widths[i] - row[i].size() + 1, ' ');
		}
		os << '\n';
	}
}

// A valid C++ identifier from an authored name (the audio-graph CUE
// names are free-form text; the emitted audiocues constants need
// identifiers). Invalid characters fold to '_'; a leading digit gets
// an '_' prefix; empty becomes "_".
std::string CppIdent(const std::string& s) {
	std::string o;
	for (char c : s) {
		const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
			|| (c >= '0' && c <= '9') || c == '_';
		o += ok ? c : '_';
	}
	if (o.empty())
		o = "_";
	if (o[0] >= '0' && o[0] <= '9')
		o = "_" + o;
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
			std::string familySource;
			if (smvk::assets::IsMeshSourceFile(fs::path(artifact)))
				familySource = artifact;
			else {
				fs::path src = fs::path(artifact);
				for (const char* e : { ".gltf", ".glb", ".obj" }) {
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
			else if (asset.kind == PlanAssetKind::Gi) {
				// GI bake SETS (.givolumemanifest — the manifest names
				// the set's .givolume payloads): the file's own sidecar
				// identity, ids in the 0x6xxx range. The runtime loads
				// these by GiKey (AssetManager::LoadGI) and can blend
				// two resident sets.
				if (!ensureAsset(asset.path, "gi", pak.id, {},
						out.gi))
					return false;
			}
			else if (asset.kind == PlanAssetKind::SpriteSet) {
				if (!ensureAsset(asset.path, "spriteset", pak.id, {}, out.spritesets))
					return false;
				std::string text;
				bool exists = false;
				if (!ReadTextFile((fs::path(project.root) / asset.path).generic_string(), exists, text) || !exists) {
					err = "spriteset read failed: " + asset.path;
					return false;
				}
				::tour::asset::SpriteSet sheet;
				std::string why;
				if (!::tour::asset::ParseSpriteSetText(text, sheet, why)) {
					err = "spriteset " + asset.path + ": " + why;
					return false;
				}
				bool found = false;
				for (const auto& other : plan.paks) {
					if (other.id != pak.id &&
						std::find(pak.depends.begin(), pak.depends.end(), other.id) == pak.depends.end()) continue;
					for (const auto& a : other.assets)
						if (a.path == sheet.texture && a.kind == PlanAssetKind::Texture) found = true;
				}
				if (!found) { err = "spriteset " + asset.path + " requires texture in its pak or a declared dependency: " + sheet.texture; return false; }
			}
			else if (asset.kind == PlanAssetKind::Shape) {
				if (!ensureAsset(asset.path, "shape", pak.id, {}, out.shapes))
					return false;
				std::string text;
				bool exists = false;
				if (!ReadTextFile(
						(fs::path(project.root) / asset.path).generic_string(),
						exists, text) || !exists) {
					err = "shape source read failed: " + asset.path;
					return false;
				}
				::tour::asset::ShapeAsset shape;
				std::string why;
				if (!::tour::asset::ParseShapeAsset(text, shape, why)) {
					err = "shape " + asset.path + ": " + why;
					return false;
				}
			}
			else if (asset.kind == PlanAssetKind::StateMachine
					|| asset.kind == PlanAssetKind::NavMesh) {
				const bool isState = asset.kind == PlanAssetKind::StateMachine;
				if (!ensureAsset(asset.path,
						isState ? "statemachine" : "navmesh", pak.id, {},
						isState ? out.statemachines : out.navmeshes))
					return false;
				std::string text;
				bool exists = false;
				if (!ReadTextFile(
						(fs::path(project.root) / asset.path).generic_string(),
						exists, text) || !exists) {
					err = "gameplay asset read failed for '" + asset.path + "'";
					return false;
				}
				std::string parseErr;
				if (isState) {
					::tour::asset::StateMachineAsset graph;
					if (!::tour::asset::ParseStateMachineText(text, graph, parseErr)) {
						err = asset.path + ": " + parseErr;
						return false;
					}
				} else {
					::tour::asset::NavMeshAsset nav;
					if (!::tour::asset::ParseNavMeshText(text, nav, parseErr)) {
						err = asset.path + ": " + parseErr;
						return false;
					}
				}
			}
			else if (asset.kind == PlanAssetKind::AudioGraph) {
				// AUTHORED AUDIO GRAPHS (.audiograph — the audio
				// system's node-graph + cue asset): the file's own
				// sidecar identity, ids in the 0x7xxx range. The text
				// is PARSED here (the loud catalog-time validation —
				// a malformed graph fails the ids pass) and the cue
				// table recorded for the header's project::audiocues
				// constants.
				if (!ensureAsset(asset.path, "audiograph", pak.id, {},
						out.audiographs))
					return false;
				std::string text;
				bool exists = false;
				if (!ReadTextFile(
						(fs::path(project.root)
							/ fs::path(asset.path)).lexically_normal()
							.generic_string(),
						exists, text)
					|| !exists) {
					err = "audiograph read failed for '" + asset.path
						+ "'";
					return false;
				}
				::tour::asset::AudioGraphDoc doc;
				std::string parseErr;
				if (!::tour::asset::ParseAudioGraphText(text, doc,
						parseErr)) {
					err = "audiograph '" + asset.path
						+ "' failed to parse: " + parseErr;
					return false;
				}
				ProjectCatalog::AudioGraphCueTable table;
				table.id = out.audiographs.back().id;
				table.symbol = out.audiographs.back().symbol;
				for (const ::tour::asset::AudioCue& cue : doc.cues)
					table.cues.push_back(cue.name);
				out.audiographCues.push_back(std::move(table));
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
	std::sort(out.gi.begin(), out.gi.end(), byId);
	std::sort(out.audiographs.begin(), out.audiographs.end(), byId);
	std::sort(out.spritesets.begin(), out.spritesets.end(), byId);
	std::sort(out.statemachines.begin(), out.statemachines.end(), byId);
	std::sort(out.navmeshes.begin(), out.navmeshes.end(), byId);
	std::sort(out.shapes.begin(), out.shapes.end(), byId);
	std::sort(out.audiographCues.begin(), out.audiographCues.end(),
		[](const ProjectCatalog::AudioGraphCueTable& a,
			const ProjectCatalog::AudioGraphCueTable& b) {
			return a.id < b.id;
		});
	return true;
}

// The catalog IDENTITY fingerprint (the shared fold, appmanifest.h's
// ComputeCatalogFingerprint): over the entries in EMISSION order
// (bucket order: scenes, textures, meshes, sounds, shaders, gi;
// id-sorted within - exactly what the header's Assets array and the
// registry's records both enumerate), folding (id, kind byte, path)
// — IDENTITY ONLY, no package addressing. The SAME implementation
// the deployment half uses - one fold, no drift between the
// compiled and deployed sides.
uint64_t CatalogFingerprintOf(const ProjectCatalog& c) {
	std::vector<uint64_t> ids;
	std::vector<uint8_t> kinds;
	std::vector<const char*> paths;
	const auto push = [&](const std::vector<CatalogEntry>& entries,
			uint8_t kind) {
		for (const CatalogEntry& e : entries) {
			ids.push_back(e.id);
			kinds.push_back(kind);
			paths.push_back(e.path.c_str());
		}
	};
	push(c.scenes, smvk::kCatalogKindScene);
	push(c.textures, smvk::kCatalogKindTexture);
	push(c.meshes, smvk::kCatalogKindMesh);
	push(c.sounds, smvk::kCatalogKindSound);
	push(c.shaders, smvk::kCatalogKindShader);
	push(c.gi, smvk::kCatalogKindGi);
	push(c.audiographs, smvk::kCatalogKindAudioGraph);
	push(c.spritesets, smvk::kCatalogKindSpriteSet);
	push(c.statemachines, smvk::kCatalogKindStateMachine);
	push(c.navmeshes, smvk::kCatalogKindNavMesh);
	push(c.shapes, smvk::kCatalogKindShape);
	return smvk::ComputeCatalogFingerprint(ids.data(), kinds.data(),
		paths.data(), ids.size());
}

std::string EmitProjectHeader(const ProjectCatalog& c,
	const GenerationStamp& stamp,
	const std::vector<AssetFingerprint>& assets,
	const std::vector<PakFingerprint>& paks) {
	std::ostringstream os;
	os << "// GENERATED FILE - DO NOT EDIT. Manual changes are\n"
		<< "// OVERWRITTEN by Tourmaline on the next project build.\n"
		<< "// Generated by Tourmaline (Project.exe ids).\n"
		<< "// The content-as-code catalog: stable ids from the project's\n"
		<< "// asset database (assets.db - the one identity store); the\n"
		<< "// same content ships in the paks below. GENERATION-STAMPED\n"
		<< "// (the version block below): the stamp moves only on a\n"
		<< "// generation event; an unchanged generation re-emits this\n"
		<< "// file byte-identical (write-only-on-change).\n"
		<< "#pragma once\n\n"
		<< "#include <array>\n#include <cstddef>\n"
			"#include <cstdint>\n#include <cstring>\n"
			"#include <string_view>\n"
		<< "// The compile-time query API (MatchAssets + the constexpr\n"
		<< "// regex engine + AssetTraits' primary) - pure C++20.\n"
		<< "#include <Engine/Asset/asset_query.h>\n\n"
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
		<< "// GI bake SETS (the 0x6xxx range - a .givolumemanifest:\n"
		<< "// a baked lighting set of one or more GI volumes, loadable\n"
		<< "// through AssetManager::LoadGI - the runtime can blend two\n"
		<< "// resident sets, see docs/content_as_code.md).\n"
		<< "struct GiKey { Id id = 0; constexpr operator Id() const"
		<< " { return id; } };\n"
		<< "// AUTHORED AUDIO GRAPHS (the 0x7xxx range - a .audiograph:\n"
		<< "// the audio system's node-graph + cue asset,\n"
		<< "// docs/audio_graph.md - loadable through the audio facade\n"
		<< "// into a runtime AudioInstance handle).\n"
		<< "struct SpriteSetKey { Id id = 0; constexpr operator Id() const { return id; } };\n"
		<< "struct StateMachineKey { Id id = 0; constexpr operator Id() const { return id; } };\n"
		<< "struct NavMeshKey { Id id = 0; constexpr operator Id() const { return id; } };\n"
		<< "struct ShapeKey { Id id = 0; constexpr operator Id() const { return id; } };\n"
		<< "struct AudioGraphKey { Id id = 0; constexpr operator Id()"
		<< " const { return id; } };\n"
		<< "// ONE CUE of an authored audio graph: the (graph id, cue\n"
		<< "// ordinal) pair (the ordinal indexes the authored file's\n"
		<< "// cue list - project::audiocues::* below). A different\n"
		<< "// thing from a KEY: this names WHICH CUE, the graph key\n"
		<< "// names WHICH ASSET.\n"
		<< "struct AudioCueKey { Id graph = 0; std::uint32_t cue = 0;"
		<< " constexpr operator bool() const { return graph != 0; } };\n"
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
		// Align the value braces within each group of names.
		size_t symbolWidth = 0;
		for (const CatalogEntry& e : entries)
			symbolWidth = std::max(symbolWidth, e.symbol.size());
		os << "namespace " << name << " {\n";
		for (const CatalogEntry& e : entries)
			os << "    inline constexpr " << keyType << " "
				<< e.symbol
				<< std::string(symbolWidth - e.symbol.size(), ' ')
				<< "{0x" << std::hex << e.id << std::dec
				<< "ULL};\n";
		os << "}\n\n";
	};
	emitNamespace("scenes", c.scenes, "SceneKey");
	emitNamespace("textures", c.textures, "TextureKey");
	emitNamespace("meshes", c.meshes, "MeshKey");
	emitNamespace("sounds", c.sounds, "SoundKey");
	emitNamespace("shaders", c.shaders, "ShaderKey");
	emitNamespace("gi", c.gi, "GiKey");
	emitNamespace("audiographs", c.audiographs, "AudioGraphKey");
	emitNamespace("spritesets", c.spritesets, "SpriteSetKey");
	emitNamespace("statemachines", c.statemachines, "StateMachineKey");
	emitNamespace("navmeshes", c.navmeshes, "NavMeshKey");
	emitNamespace("shapes", c.shapes, "ShapeKey");

	// ---- THE PER-CUE CONSTANTS (project::audiocues) --------------------
	// Which CUE of WHICH authored graph: one constant per (graph,
	// cue), the name <graphSymbol>_<cueName>. OUTSIDE the runtime
	// catalog ABI (like AssetSymbols): the cue ordinal is authored
	// file order, stable for a given graph text; a cue edit is a
	// generation event (the fingerprints + the content hash cover the
	// file bytes).
	if (!c.audiographCues.empty()) {
		os << "// The authored audio-graph CUES (docs/audio_graph.md):\n"
			<< "// each constant bundles (graph id, cue ordinal) - the\n"
			<< "// ordinal indexes the authored file's cue list. Feed\n"
			<< "// these to the audio facade's PlayCue/CrossFade.\n"
			<< "namespace audiocues {\n";
		size_t cueWidth = 0;
		for (const auto& table : c.audiographCues)
			for (const std::string& cue : table.cues)
				cueWidth = std::max(cueWidth,
					table.symbol.size() + 1 + CppIdent(cue).size());
		for (const auto& table : c.audiographCues)
			for (size_t ci = 0; ci < table.cues.size(); ++ci) {
				const std::string name = table.symbol + "_"
					+ CppIdent(table.cues[ci]);
				os << "    inline constexpr AudioCueKey " << name
					<< std::string(cueWidth - name.size(), ' ')
					<< "{0x" << std::hex << table.id << std::dec
					<< "ULL, " << ci << "u};\n";
			}
		os << "} // namespace audiocues\n\n";
	}

	// ---- the generated TYPES (the node schemas; "compile types;
	// cook values") -----------------------------------------------
	// The C++ layout is GUARANTEED to equal the cooked layout: fields
	// in declaration order, naturally aligned (a vec3 aligns 4 like
	// the emitted float[3]; ref aligns 8) - the static_asserts prove
	// every field offset + the fixed prefix's extent. str fields are
	// layout-TERMINAL (the variable tail): they are NOT members; the
	// generated accessor reads them past the fixed prefix.
	if (!c.schemas.empty()) {
		os << "// THE GENERATED TYPES (the project's node schemas -\n"
			<< "// docs/content_as_code.md). COMPILE TYPES; COOK VALUES:\n"
			<< "// the layout below equals the cooked value block the\n"
			<< "// scene's v19 schema-association rows carry (asserted);\n"
			<< "// FromValues decodes a row's bytes with one memcpy.\n"
			<< "// Decode str fields through the generated accessors\n"
			<< "// (they live past the fixed prefix - the variable tail).\n"
			<< "namespace types {\n\n";
		// Id-sorted for determinism (same order as the content hash).
		std::vector<const NodeSchema*> sortedSchemas;
		sortedSchemas.reserve(c.schemas.size());
		for (const NodeSchema& s : c.schemas)
			sortedSchemas.push_back(&s);
		std::sort(sortedSchemas.begin(), sortedSchemas.end(),
			[](const NodeSchema* a, const NodeSchema* b) {
				return a->id < b->id;
			});
		for (const NodeSchema* sp : sortedSchemas) {
			const NodeSchema& s = *sp;
			os << "struct " << s.name << " {\n"
				<< "    static constexpr std::uint32_t SchemaId = "
				<< s.id << "u;\n";
			// The fixed members (everything except the terminal str).
			uint32_t fixedBytes = 0;
			const SchemaField* strField = nullptr;
			std::vector<std::string> asserts;
			for (const SchemaField& f : s.fields) {
				if (f.type == SchemaFieldType::Str) {
					strField = &f;
					continue;
				}
				os << "    "; // member
				switch (f.type) {
				case SchemaFieldType::F32:
					os << "float " << f.name; break;
				case SchemaFieldType::I32:
					os << "std::int32_t " << f.name; break;
				case SchemaFieldType::U32:
				case SchemaFieldType::Bool:
					os << "std::uint32_t " << f.name; break;
				case SchemaFieldType::Ref:
					os << "std::uint64_t " << f.name; break;
				case SchemaFieldType::Vec2:
				case SchemaFieldType::Vec3:
				case SchemaFieldType::Vec4: {
					const uint32_t n = f.stride / 4;
					os << "float " << f.name;
					if (f.count > 1)
						os << "[" << f.count << "]";
					os << "[" << n << "]";
					break;
				}
				default:
					break;
				}
				if (f.count > 1
						&& f.type != SchemaFieldType::Vec2
						&& f.type != SchemaFieldType::Vec3
						&& f.type != SchemaFieldType::Vec4)
					os << "[" << f.count << "]";
				os << "; // field " << f.id << "\n";
				asserts.push_back("static_assert(offsetof(" + s.name
					+ ", " + f.name + ") == " + std::to_string(f.offset)
					+ "u);");
				fixedBytes = f.offset + f.stride * f.count;
			}
			// The C++ sizeof includes tail padding (alignment); the
			// FIXED extent is the last fixed field's end - asserted
			// through the last member, never sizeof.
			os << "    static constexpr std::uint32_t FixedBytes = "
				<< fixedBytes << "u;\n"
				<< "    static " << s.name
				<< " FromValues(const void* cooked) {\n"
				<< "        " << s.name << " v{};\n"
				<< "        std::memcpy(&v, cooked, FixedBytes);\n"
				<< "        return v;\n"
				<< "    }\n";
			if (strField) {
				os << "    // The layout-terminal str tail (the\n"
					<< "    // variable-length end of the row's cooked\n"
					<< "    // block - never a member).\n"
					<< "    static std::string_view " << strField->name
					<< "(const std::uint8_t* rowBytes,\n"
					<< "            std::uint32_t rowBytesCount) {\n"
					<< "        if (rowBytesCount < "
					<< (strField->offset + 4) << "u)\n"
					<< "            return {};\n"
					<< "        std::uint32_t len = 0;\n"
					<< "        std::memcpy(&len, rowBytes + "
					<< strField->offset << "u, 4);\n"
					<< "        if (rowBytesCount < "
					<< (strField->offset + 4) << "u + len)\n"
					<< "            return {};\n"
					<< "        return { reinterpret_cast<const char*>"
					<< "(rowBytes + " << (strField->offset + 4)
					<< "u), len };\n"
					<< "    }\n";
			}
			os << "};\n";
			for (const std::string& a : asserts)
				os << a << "\n";
			os << "\n";
		}
		os << "} // namespace types\n\n";
	}

	os << "// Runtime packages (plan/mount order).\n"
		<< "struct Pak {\n"
		<< "    std::string_view name;\n"
		<< "    std::string_view file;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Paks = {\n";
	{
		std::vector<std::vector<std::string>> rows;
		rows.reserve(c.paks.size());
		for (const auto& [id, file] : c.paks)
			rows.push_back({ "Pak{\"" + CppStr(id) + "\",",
				"\"" + CppStr(file) + "\"}," });
		EmitAlignedRows(os, rows);
	}
	os << "};\n\n"
		<< "// The asset KIND enum - the values are an ABI (the runtime's\n"
		<< "// catalog-fingerprint fold + the .tapp validation agree on\n"
		<< "// these bytes; zero is Nil, real kinds start at one).\n"
		<< "enum class AssetKind : std::uint8_t {\n";
	// Enum labels are also a two-column collection: align their '='.
	EmitAlignedRows(os, {
		{ "Nil", "= 0," },
		{ "Scene", "= 1," },
		{ "Texture", "= 2," },
		{ "Mesh", "= 3," },
		{ "Sound", "= 4," },
		{ "Shader", "= 5," },
		{ "Gi", "= 6," },
		{ "AudioGraph", "= 7," },
		{ "SpriteSet", "= 8," },
		{ "StateMachine", "= 9," },
		{ "NavMesh", "= 10," },
		{ "Shape", "= 11," }
	});
	os << "};\n\n"
		<< "// The FLAT TYPED CATALOG: plain compile-time POD records -\n"
		<< "// no startup strings, no maps, no runtime discovery table.\n"
		<< "// IDENTITY ONLY (id, kind, path): the owning package, its\n"
		<< "// mount index and the published filename are SELECTION\n"
		<< "// state, never identity - the content registry answers\n"
		<< "// \"who supplies it\" at runtime. sceneName: the cooked-scene\n"
		<< "// identity is cook-state, not stable identity, so it stays\n"
		<< "// deployment-side - the field is reserved and emitted empty.\n"
		<< "struct AssetDesc {\n"
		<< "    Id id = 0;\n"
		<< "    AssetKind kind = AssetKind::Nil;\n"
		<< "    std::string_view path;\n"
		<< "    std::string_view sceneName;\n"
		<< "};\n\n"
		<< "inline constexpr std::array Assets = {\n";
	{
		std::vector<std::vector<std::string>> rows;
		const auto addRows = [&](const std::vector<CatalogEntry>& entries,
				const char* ns, const char* kind) {
			for (const CatalogEntry& e : entries)
				rows.push_back({
					std::string("AssetDesc{") + ns + "::" + e.symbol + ",",
					std::string("AssetKind::") + kind + ",",
					"\"" + CppStr(e.path) + "\",",
					"\"\"},"
				});
		};
		addRows(c.scenes, "scenes", "Scene");
		addRows(c.textures, "textures", "Texture");
		addRows(c.meshes, "meshes", "Mesh");
		addRows(c.sounds, "sounds", "Sound");
		addRows(c.shaders, "shaders", "Shader");
		addRows(c.gi, "gi", "Gi");
		addRows(c.audiographs, "audiographs", "AudioGraph");
		addRows(c.spritesets, "spritesets", "SpriteSet");
		addRows(c.statemachines, "statemachines", "StateMachine");
		addRows(c.navmeshes, "navmeshes", "NavMesh");
		addRows(c.shapes, "shapes", "Shape");
		EmitAlignedRows(os, rows);
	}
	os << "};\n\n"
		<< "// The SYMBOL-NAME table (compile-time query metadata; the\n"
		<< "// same order as Assets). NOT part of the runtime catalog\n"
		<< "// ABI - AssetDesc/CompiledAssetDesc/the fingerprint fold\n"
		<< "// are untouched; only MatchAssets reads this.\n"
		<< "inline constexpr std::array AssetSymbols = {\n";
	const auto emitSymbols = [&](
			const std::vector<CatalogEntry>& entries) {
		for (const CatalogEntry& e : entries)
			os << "    std::string_view(\"" << CppStr(e.symbol)
				<< "\"),\n";
	};
	emitSymbols(c.scenes);
	emitSymbols(c.textures);
	emitSymbols(c.meshes);
	emitSymbols(c.sounds);
	emitSymbols(c.shaders);
	emitSymbols(c.gi);
	emitSymbols(c.audiographs);
	emitSymbols(c.spritesets);
	emitSymbols(c.statemachines);
	emitSymbols(c.navmeshes);
	emitSymbols(c.shapes);
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
			if (kind == "spriteset") return "SpriteSetKey";
			if (kind == "statemachine") return "StateMachineKey";
			if (kind == "navmesh") return "NavMeshKey";
			if (kind == "shape") return "ShapeKey";
			return "Id"; // an unknown kind degrades to the raw id
		};
		os << "// RETIRED assets (tombstones): the historical identities,\n"
			<< "// typed keys with their values PRESERVED (stable API\n"
			<< "// compatibility); never in the active namespaces, never\n"
			<< "// reused. A runtime load of a retired id fails (the\n"
			<< "// compiled catalog carries no row for it).\n"
			<< "namespace retired {\n";
		size_t typeWidth = 0;
		size_t symbolWidth = 0;
		for (const CatalogEntry& e : c.retired) {
			typeWidth = std::max(typeWidth,
				std::strlen(keyTypeOf(e.kind)));
			symbolWidth = std::max(symbolWidth, e.symbol.size());
		}
		for (const CatalogEntry& e : c.retired) {
			const std::string kt = keyTypeOf(e.kind);
			os << "    inline constexpr " << kt
				<< std::string(typeWidth - kt.size() + 1, ' ')
				<< e.symbol
				<< std::string(symbolWidth - e.symbol.size(), ' ')
				<< "{0x" << std::hex << e.id << std::dec << "ULL};\n";
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

	// ---- THE COMPILE-TIME QUERY API BINDING (MatchAssets) ----------
	// The catalog carrier (the query core's data seam - static
	// constexpr accessors; no object NTTPs). MatchAssets<Key,
	// "pattern"> filters Assets by kind + a constexpr REGEX over the
	// SYMBOL names (default) or the content PATHS (MatchField::Path),
	// returning a precisely sized std::array<Key, N> in catalog
	// order - see Engine/Asset/asset_query.h for the grammar.
	os << "// The catalog carrier + the compile-time query binding.\n"
		<< "struct AssetCatalog {\n"
		<< "    static constexpr auto Rows() { return Assets; }\n"
		<< "    static constexpr auto Symbols() { return AssetSymbols; }\n"
		<< "};\n"
		<< "template<typename Key,\n"
		<< "    ::project_pipeline::query::FixedString Pattern,\n"
		<< "    ::project_pipeline::query::MatchField Field\n"
		<< "        = ::project_pipeline::query::MatchField::Symbol>\n"
		<< "consteval auto MatchAssets() {\n"
		<< "    return ::project_pipeline::query::MatchAssetsImpl<\n"
		<< "        Key, Pattern, Field, AssetCatalog>();\n"
		<< "}\n\n";
	os << "} // namespace project\n\n";

	// The AssetTraits specializations (OUTER scope - the qualified-id
	// form the standard requires for a cross-namespace
	// specialization; the query core consults these at instantiation
	// to filter by the row's catalog kind).
	os << "// The kind traits (MatchAssets' typed filter).\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::SceneKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Scene; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::TextureKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Texture; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::MeshKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Mesh; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::SoundKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Sound; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::ShaderKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Shader; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::GiKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::Gi; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::AudioGraphKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::AudioGraph; }\n"
		<< "};\n"
		<< "\n";

	os << "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::SpriteSetKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::SpriteSet; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::StateMachineKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::StateMachine; }\n"
		<< "};\n"
		<< "template<>\n"
		<< "struct project_pipeline::query::AssetTraits<project::NavMeshKey> {\n"
		<< "    static constexpr bool Matches(project::AssetKind k)\n"
		<< "        { return k == project::AssetKind::NavMesh; }\n"
		<< "};\n\n";

	// The shader interface supplement is optional: projects that compile
	// local HLSL can expose the reflection constants without modifying the
	// stable assets.db catalog or its .tapp fingerprint.
	os << "// Optional build-generated local shader reflection catalog.\n"
		<< "#if __has_include(\"project_compiled_shaders.h\")\n"
		<< "#include \"project_compiled_shaders.h\"\n"
		<< "#endif\n\n";

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
	addBucket("gi", c.gi);
	addBucket("audiograph", c.audiographs);
	addBucket("spriteset", c.spritesets);
	addBucket("statemachine", c.statemachines);
	addBucket("navmesh", c.navmeshes);
	addBucket("shape", c.shapes);
	// The node schemas are catalog SEMANTICS ("compile types; cook
	// values"): a schema edit changes the generated TYPES — a
	// generation event, never a silent re-emit.
	canonical += "schemas\n";
	{
		std::vector<const NodeSchema*> sorted;
		sorted.reserve(c.schemas.size());
		for (const NodeSchema& s : c.schemas)
			sorted.push_back(&s);
		std::sort(sorted.begin(), sorted.end(),
			[](const NodeSchema* a, const NodeSchema* b) {
				return a->id < b->id;
			});
		for (const NodeSchema* s : sorted) {
			canonical += "schema " + std::to_string(s->id) + " "
				+ s->name + " " + std::to_string(s->valueBytes)
				+ "\n";
			for (const SchemaField& f : s->fields) {
				canonical += "field " + std::to_string(f.id) + " "
					+ f.name + " "
					+ std::to_string(uint32_t(f.type)) + " "
					+ std::to_string(f.count) + " "
					+ std::to_string(f.offset) + " "
					+ std::to_string(f.stride) + " "
					+ std::to_string(f.defaultValue.size())
					+ "\n";
				canonical.insert(canonical.end(),
					f.defaultValue.begin(), f.defaultValue.end());
				canonical += "\n";
			}
		}
	}
	// The retired set is catalog SEMANTICS too: a retire/reactivate
	// moves the content hash (a generation event), so the compiled
	// catalog and its hash can never disagree about membership.
	canonical += "retired\n";
	for (const CatalogEntry& e : c.retired)
		canonical += "asset 0x" + Hex64(e.id) + " " + e.symbol + " "
			+ e.kind + "\n";
	return smvk::cache::HashBytes(canonical.data(), canonical.size());
}

bool CollectNodeSchemas(const std::string& projectRoot,
	std::vector<NodeSchema>& out, std::string& err) {
	out.clear();
	// The project IS the content tree: every .nodeschema file under
	// the root is a project schema (recursive, deterministic walk).
	namespace fs = std::filesystem;
	std::error_code ec;
	std::vector<std::string> files;
	for (fs::recursive_directory_iterator it(projectRoot, ec), end;
			!ec && it != end; it.increment(ec)) {
		if (!it->is_regular_file(ec))
			continue;
		if (it->path().extension().generic_string() != ".nodeschema")
			continue;
		files.push_back(it->path().generic_string());
	}
	if (ec) {
		err = "the schema walk failed under '" + projectRoot + "': "
			+ ec.message();
		return false;
	}
	std::sort(files.begin(), files.end());
	for (const std::string& file : files) {
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			err = "cannot open schema file: " + file;
			return false;
		}
		const std::string text((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		std::vector<NodeSchema> parsed;
		std::string perr;
		if (!ParseNodeSchemaFile(text, parsed, perr)) {
			err = "the schema file '" + file + "' failed: " + perr;
			return false;
		}
		for (NodeSchema& s : parsed)
			out.push_back(std::move(s));
	}
	// Id-sorted + duplicate-id refusal (two schemas claiming one id
	// would silently shadow in every id-keyed lookup).
	std::sort(out.begin(), out.end(),
		[](const NodeSchema& a, const NodeSchema& b) {
			return a.id < b.id;
		});
	for (size_t i = 1; i < out.size(); ++i)
		if (out[i].id == out[i - 1].id) {
			err = "two schemas claim id " + std::to_string(out[i].id)
				+ " ('" + out[i - 1].name + "' and '" + out[i].name
				+ "') - schema ids are stable and unique";
			return false;
		}
	return true;
}

bool EmitPlansHeader(const ProjectCatalog& c, const std::string& pakDir,
	std::string& text, std::string& err) {
	namespace fs = std::filesystem;
	text.clear();
	// Open every manifest pak (the light SMP8 mount — no blob bytes;
	// the TOC + the mesh plan only).
	std::vector<std::string> pakFingerprints;
	std::vector<std::optional<smvk::PakReader>> readers;
	size_t present = 0;
	for (const auto& [pakId, pakFile] : c.paks) {
		(void)pakId;
		const fs::path path = fs::path(pakDir) / pakFile;
		std::string oerr;
		auto reader = smvk::PakReader::OpenFile(
			path.generic_string(), oerr);
		if (reader) {
			++present;
			pakFingerprints.push_back(
				smvk::PakLayoutFingerprint(*reader));
		}
		else {
			pakFingerprints.emplace_back();
		}
		readers.push_back(std::move(reader));
	}
	if (present != 0 && present != readers.size()) {
		err = "the plans emission found an INCONSISTENT publish: "
			+ std::to_string(present) + " of "
			+ std::to_string(readers.size())
			+ " manifest paks open under '" + pakDir
			+ "' - redeploy the project (never a partial-plan"
			" header)";
		return false;
	}
	if (present == 0) {
		// A fresh tree before the first build: the EMPTY header. The
		// gate refuses any deployed pak against PlansPakCount 0 —
		// fail-closed by construction.
		text = "// Generated by Tourmaline (Project.exe ids). Do not"
			" edit.\n"
			"// THE COMPILED STATIC PLANS - PRIVATE (never the"
			" game-facing project.h; see Engine/Asset/asset_plan.h).\n"
			"// No published paks yet: the empty form. The first"
			" <Name>_paks build fills it; the runtime gate refuses"
			" any deployed pak against it until then.\n"
			"#pragma once\n\n"
			"#include <Engine/Asset/asset_plan.h>\n\n"
			"namespace project::plans {\n"
			"inline constexpr std::uint32_t PlansPakCount = 0;\n"
			"inline constexpr const char* PlansLayoutHash = \"\";\n"
			"} // namespace project::plans\n";
		return true;
	}
	// The per-mesh plan constants: id-sorted (the catalog's
	// determinism), each resolved through its pak's mesh plan to the
	// family's BASE slot — a catalog mesh the plan does not carry is
	// a loud version-skew failure.
	std::vector<CatalogEntry> meshes(c.meshes);
	std::sort(meshes.begin(), meshes.end(),
		[](const CatalogEntry& a, const CatalogEntry& b) {
			return a.id < b.id;
		});
	std::ostringstream os;
	os << "// Generated by Tourmaline (Project.exe ids). Do not edit.\n"
		<< "// THE COMPILED STATIC PLANS - PRIVATE (never the\n"
		<< "// game-facing project.h; see Engine/Asset/asset_plan.h).\n"
		<< "// One MeshPlanRef per mesh asset: the [paks] mount index\n"
		<< "// + the family's BASE pak slot + the .geometry identity.\n"
		<< "// The layout hash folds every assumption the compiled\n"
		<< "// reads make (pak list, slot tables, the mesh-plan join)\n"
		<< "// - content bytes are NOT part of it: a payload-only\n"
		<< "// redeploy with an unchanged layout needs NO recompile.\n"
		<< "// The runtime gate (ProjectRuntime::AttachCompiledPlans)\n"
		<< "// compares the DEPLOYED paks against it and FAILS CLOSED.\n"
		<< "#pragma once\n\n"
		<< "#include <cstdint>\n"
		<< "#include <Engine/Asset/asset_plan.h>\n\n"
		<< "namespace project::plans {\n\n"
		<< "inline constexpr std::uint32_t PlansPakCount = "
		<< readers.size() << "u;\n";
	std::vector<std::string> pakIds;
	for (const auto& [pakId, pakFile] : c.paks)
		pakIds.push_back(pakId);
	const std::string layoutHash = smvk::FoldPakLayouts(
		pakIds.data(), pakFingerprints.data(), pakIds.size());
	os << "inline constexpr const char* PlansLayoutHash = \""
		<< layoutHash << "\";\n\n";
	for (const CatalogEntry& e : meshes) {
		// The mesh's pak (mount index) — the catalog row names the
		// logical pak id.
		size_t pakIndex = SIZE_MAX;
		for (size_t i = 0; i < pakIds.size(); ++i)
			if (pakIds[i] == e.pak) {
				pakIndex = i;
				break;
			}
		if (pakIndex == SIZE_MAX) {
			err = "the mesh asset '" + e.symbol
				+ "' names pak '" + e.pak
				+ "' but the plan's pak list does not carry it"
				" (a resolver skew - rebuild the ids)";
			return false;
		}
		const smvk::PakReader& reader = *readers[pakIndex];
		std::vector<smvk::PakMeshPlanMember> members;
		std::string merr;
		if (!reader.MeshPlanFamily(e.path, members, merr)) {
			err = "the mesh asset '" + e.symbol + "' (" + e.path
				+ ") has no family in pak '" + e.pak
				+ "'s mesh plan: " + merr
				+ " (a version skew between the catalog and the"
				" cooked content - recook)";
			return false;
		}
		const smvk::PakTocEntry* base = nullptr;
		for (const smvk::PakMeshPlanMember& m : members) {
			if (m.lodIndex != 0)
				continue;
			base = reader.Find(smvk::PakAssetKind::Mesh, m.tocPath);
			break;
		}
		if (!base) {
			err = "the mesh asset '" + e.symbol + "' (" + e.path
				+ "): the pak's mesh plan carries no BASE member"
				" (a cook anomaly - recook)";
			return false;
		}
		os << "inline constexpr smvk::MeshPlanRef " << e.symbol
			<< "{ " << pakIndex << "u, " << base->slot << "u, \""
			<< CppStr(e.path) << "\" };\n";
	}
	os << "\n} // namespace project::plans\n";
	text = os.str();
	return true;
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
		&& add(catalog.shaders) && add(catalog.audiographs)
		&& add(catalog.spritesets)
		&& add(catalog.statemachines) && add(catalog.navmeshes)
		&& add(catalog.shapes);
}

bool DiscoverPakFingerprints(const std::string& pakDir,
	const ProjectCatalog& catalog,
	const std::vector<AssetFingerprint>& assets,
	const HeaderMetadata& prev, std::vector<PakFingerprint>& out,
	std::string& err) {
	err.clear();
	out.clear();
	// 1) the LIVE pak set: every published pak's OWN "#ids" identity
	//    table joins STABLE asset ids to pak FILES — never file-name
	//    guessing (the content-addressed names carry digests the
	//    catalog pass cannot predict), never a manifest mirror (the
	//    [ids] table died with appmanifest 2 — the paks are the
	//    authority, same as the runtime's discovery lane). A pak that
	//    fails its light mount or carries no table names nothing
	//    (not ours / legacy — the fallback paths stay).
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
				if (e.path().extension().generic_string() != ".pak")
					continue;
				const std::string pakPath =
					e.path().lexically_normal().generic_string();
				::smvk::PakPager pager(pakPath);
				if (!pager.Valid())
					continue; // corrupt/foreign: names nothing
				const ::tour::asset::PakTocEntry* entry =
					pager.Reader().Find(
						::tour::asset::PakAssetKind::Identity,
						::tour::asset::kPakAssetIdsPath);
				if (!entry)
					continue; // a legacy/asset-only pak: no rows
				std::string perr;
				::smvk::PakPager::PinnedPage page =
					pager.Read(*entry, perr);
				if (!page)
					continue;
				std::vector<::tour::asset::PakAssetIdRow> rows;
				if (!::tour::asset::ParseAssetIdsBlob(page.bytes(),
						rows, perr))
					continue;
				for (const ::tour::asset::PakAssetIdRow& row : rows)
					pakFileByAssetId[row.id] =
						e.path().filename().generic_string();
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
		//          identity row in any published pak — the pak cannot
		//          contain it),
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
