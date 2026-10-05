// Path editor: consolidated view over every file/path the scene depends
// on (File > Path editor). Solves missing paths and easy repaths —
// notably broken/missing texture links when bringing a glTF whose image
// URIs no longer resolve.
//
// Sections: scene source + tour, scene references, imported image URIs
// (the texture core — the IMPORTER'S data records, never a kept parsed
// glTF), editor image definitions, and outputs (cook pak, lightmap
// atlas, env skybox).
//
// Texture fixes, one persistent mechanism: the relink is an INPUT PLUG
// on the source asset (plug.texture.* rows in its .meta sidecar — see
// artifact_cache's AssetPlug). The importer applies plugs at EVERY
// import (editor live import, PakCooker cook, GiBaker bake), so one
// relink fixes the viewport immediately (live material-slot updates via
// undoable commands) AND every future import/cook/bake — the old
// editor-only path.remap.* tour settings never cooked and are gone.
// "To expected" stays as the file-copy alternative (place the file
// where the glTF URI points).

#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

#include <imgui.h>

#include <artifact_cache/assetdb.h>

namespace editor {

namespace {

bool PathExists(const std::string& p) {
	if (p.empty())
		return false;
	std::error_code ec;
	return std::filesystem::exists(std::filesystem::path(p), ec);
}

std::string BaseName(const std::string& p) {
	const size_t s = p.find_last_of("/\\");
	return (s == std::string::npos) ? p : p.substr(s + 1);
}

bool IsEmbeddedUri(const std::string& uri) {
	return uri.empty() || uri.rfind("data:", 0) == 0;
}

} // namespace

// Material by its registered cluster uri (import records carry the
// FULL material uri — "<source>#mat.N" — so the resolution is exact,
// unlike the old gltf-material-index suffix matching).
int EditorApp::MatWorldForUri(const std::string& uri) const {
	if (!m_world || uri.empty())
		return -1;
	for (uint32_t i = 0; i < uint32_t(m_world->materialLocations.size()); ++i) {
		const auto& loc = m_world->materialLocations[i];
		if (!loc.cluster || !loc.handle.valid())
			continue;
		if (std::string(loc.cluster->GetMaterialUri(loc.handle).Str()) == uri)
			return int(i);
	}
	return -1;
}

// Texture plugs of one source asset (its .meta rows; kind "texture"
// only). Straight read each call — the rows are tiny and the panel is
// the only consumer, so no cache layer to invalidate.
std::vector<smvk::cache::AssetPlug> EditorApp::TexturePlugsOf(
	const std::string& source) {
	std::vector<smvk::cache::AssetPlug> out;
	for (const smvk::cache::AssetPlug& p : smvk::cache::ReadAssetPlugs(source))
		if (p.kind == "texture")
			out.push_back(p);
	return out;
}

void EditorApp::WriteTexturePlug(const std::string& source,
	const std::string& oldUri, const std::string& newPath) {
	// Identity first: plugs ride the source's .meta, so it must carry a
	// guid (adopt the source asset when missing — sidecars travel with
	// the file; the assetdb row is a regenerable cache). Sources outside
	// a project keep working: the sidecar is written regardless.
	std::string guid;
	std::string err;
	if (!smvk::cache::ReadAssetMeta(source, guid, err))
		AdoptProjectAsset(source); // issues + writes the .meta (see editor_project.cpp)
	// Upsert the (oldUri) row among this source's texture plugs.
	std::vector<smvk::cache::AssetPlug> plugs = TexturePlugsOf(source);
	bool known = false;
	for (smvk::cache::AssetPlug& p : plugs)
		if (p.oldUri == oldUri) {
			p.newPath = newPath;
			known = true;
		}
	if (!known)
		plugs.push_back(smvk::cache::AssetPlug{ "texture", oldUri, newPath });
	if (!smvk::cache::WriteAssetPlugs(source, "texture", plugs, err))
		Log("Path editor: plug write failed ('" + source + ".meta'): "
			+ err);
}

void EditorApp::ForgetTexturePlug(const std::string& source,
	const std::string& oldUri) {
	std::vector<smvk::cache::AssetPlug> plugs;
	for (const smvk::cache::AssetPlug& p : TexturePlugsOf(source))
		if (p.oldUri != oldUri)
			plugs.push_back(p);
	std::string err;
	if (!smvk::cache::WriteAssetPlugs(source, "texture", plugs, err))
		Log("Path editor: plug forget failed ('" + source + ".meta'): "
			+ err);
	else
		Log("Path editor: plug forgotten ('" + oldUri + "' on '"
			+ source + "') — future imports read the authored uri.");
}

// Interactive relink of one imported image record: loads newPath (or
// reuses the registered texture for it), reassigns every RECORDED
// (material, slot) user via undoable commands, and writes the input
// plug on the source asset's .meta (persistent — every future import,
// cook and bake reads the replacement).
int EditorApp::RelinkTextureLive(const GltfImportImage& rec,
	const std::string& newPath) {
	if (!m_world || rec.uri.empty() || newPath.empty())
		return -1;
	smvk::Cluster* cl = m_world->clusters.empty()
		? nullptr
		: m_world->clusters[0].get();
	if (!cl)
		return -1;
	int pool = -1;
	for (const auto& [uri, th] : cl->ListTextureUris()) {
		const std::string u = uri.Str();
		if (u == newPath || BaseName(u) == BaseName(newPath)) {
			pool = int(th.index());
			break;
		}
	}
	if (pool < 0)
		pool = AddTextureFromFile(newPath);
	if (pool < 0)
		return -1;
	int touched = 0;
	for (const GltfImportImage::User& u : rec.users) {
		const int mw = MatWorldForUri(u.materialUri);
		if (mw < 0)
			continue;
		glm::vec4 v4;
		double num = 0.0;
		ReadSharedMatAttr(uint32_t(mw), u.slot, v4, num);
		if (int(num) == pool)
			continue;
		EditCmd c{};
		c.verb = EditVerb::SetMatAttr;
		c.matWorld = uint32_t(mw);
		c.attr = u.slot;
		c.beforeVec4 = v4;
		c.beforeNum = num;
		c.vec4 = v4;
		c.number = double(pool);
		Do(c);
		++touched;
	}
	// The persistent half: the input plug on the source asset.
	WriteTexturePlug(rec.source, rec.uri, newPath);
	ClearTexPreview();
	Log("Path editor: relinked '" + rec.uri + "' -> '" + newPath
		+ "' (" + std::to_string(touched) + " slot(s), pool "
		+ std::to_string(pool) + "; plug saved to '"
		+ rec.source + ".meta').");
	return pool;
}

void EditorApp::DrawPathsPanel() {
	if (!m_showPathsPanel)
		return;
	ImGui::SetNextWindowSize(ImVec2(860, 560), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Path editor", &m_showPathsPanel)) {
		ImGui::End();
		return;
	}
	if (!m_world) {
		ImGui::TextDisabled("No scene. File > Open file... (.gltf/.tour)");
		ImGui::End();
		return;
	}
	namespace fs = std::filesystem;
	const std::string gltfDir = [&]() {
		const std::string base =
			!m_gltfHostPath.empty() ? m_gltfHostPath : m_gltfPath;
		try {
			return fs::path(base).parent_path().generic_string();
		}
		catch (...) {
			return std::string();
		}
	}();
	auto expectedFor = [&](const std::string& uri) {
		if (IsEmbeddedUri(uri))
			return std::string();
		try {
			fs::path p(uri);
			if (p.is_absolute())
				return p.generic_string();
			return (fs::path(gltfDir) / p).lexically_normal().generic_string();
		}
		catch (...) {
			return uri;
		}
	};

	// ---- filter ------------------------------------------------------
	{
		int f = m_pathFilter;
		ImGui::TextDisabled("show:");
		ImGui::SameLine();
		if (ImGui::RadioButton("All", &f, 0))
			m_pathFilter = f;
		ImGui::SameLine();
		if (ImGui::RadioButton("Missing only", &f, 1))
			m_pathFilter = f;
	}
	const bool missingOnly = (m_pathFilter == 1);

	// ---- scene -------------------------------------------------------
	if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::BulletText("source: %s",
			m_gltfPath.empty() ? "(none)" : m_gltfPath.c_str());
		const bool hostOk = PathExists(m_gltfHostPath);
		if (m_gltfHostPath != m_gltfPath)
			ImGui::BulletText("file: %s  [%s]", m_gltfHostPath.c_str(),
				hostOk ? "ok" : "MISSING");
		ImGui::BulletText("tour: %s", TourTarget().c_str());
		if (ImGui::SmallButton("Open file...")) {
			std::string picked;
			if (NativeOpenFile(picked) && !picked.empty())
				OpenAnySceneFile(picked);
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy source path"))
			ImGui::SetClipboardText(m_gltfPath.c_str());
	}

	// ---- references --------------------------------------------------
	if (ImGui::CollapsingHeader("References",
			ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_references.empty()) {
			ImGui::TextDisabled("No references (File > Add scene reference...).");
		}
		else {
			int pendingReplace = -1;
			int pendingReload = -1;
			if (ImGui::BeginTable("##paths_refs", 4,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
						ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
					ImVec2(0, 110))) {
				ImGui::TableSetupColumn("Name");
				ImGui::TableSetupColumn("Source");
				ImGui::TableSetupColumn("Status",
					ImGuiTableColumnFlags_WidthFixed, 90.0f);
				ImGui::TableSetupColumn("Actions",
					ImGuiTableColumnFlags_WidthFixed, 170.0f);
				ImGui::TableHeadersRow();
				for (size_t i = 0; i < m_references.size(); ++i) {
					const auto& r = m_references[i];
					std::error_code ec;
					bool ok = fs::exists(fs::path(r.source), ec);
					if (!ok && !m_gltfHostPath.empty())
						ok = fs::exists(
							fs::path(m_gltfHostPath).parent_path() /
								fs::path(r.source),
							ec);
					const std::string resolved = (i < m_refResolved.size())
						? m_refResolved[i]
						: std::string();
					if (missingOnly && (ok || r.root))
						continue;
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(int(i));
					ImGui::Text("%s%s", r.name.c_str(),
						r.root ? " (root)" : "");
					ImGui::TableSetColumnIndex(1);
					ImGui::TextDisabled("%s", r.source.c_str());
					if (!resolved.empty() && resolved != r.source)
						ImGui::TextDisabled("-> %s", resolved.c_str());
					ImGui::TableSetColumnIndex(2);
					if (r.root)
						ImGui::TextDisabled("source");
					else if (!r.enabled)
						ImGui::TextDisabled("hidden");
					else if (!ok)
						ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
							"missing");
					else
						ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
							"ok");
					ImGui::TableSetColumnIndex(3);
					if (!r.root) {
						if (ImGui::SmallButton("Replace..."))
							pendingReplace = int(i);
						ImGui::SameLine();
						if (ImGui::SmallButton("Reload"))
							pendingReload = int(i);
					}
					else {
						ImGui::TextDisabled("the scene itself");
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (pendingReplace >= 0) {
				std::string picked;
				if (NativeOpenFile(picked) && !picked.empty() &&
					size_t(pendingReplace) < m_references.size()) {
					m_references[size_t(pendingReplace)].source = picked;
					// A new source means new identity: never keep the old
					// GUID (empty without a project = path-only, as before).
					m_references[size_t(pendingReplace)].guid =
						ProjectAssetGuid(picked);
					SaveTour();
					Log("Path editor: reference replaced: '" + picked +
						"' — reloading to rebind.");
					if (!m_gltfPath.empty())
						ReloadReferencesViaTemp();
				}
			}
			if (pendingReload >= 0 && !m_gltfPath.empty() &&
				size_t(pendingReload) < m_references.size()) {
				Log("Path editor: reloading reference '" +
					m_references[size_t(pendingReload)].source + "'.");
				ReloadReferencesViaTemp();
			}
		}
	}

	// ---- textures (the imported-image core) --------------------------
	if (ImGui::CollapsingHeader("Textures (imported images)",
			ImGuiTreeNodeFlags_DefaultOpen)) {
		if (m_importImages.empty()) {
			ImGui::TextDisabled(
				"No imported image records (open a source scene / bind references).");
		}
		else {
			smvk::Cluster* cl = m_world->clusters.empty()
				? nullptr
				: m_world->clusters[0].get();
			// One row per import record: the authored uri, the resolved
			// source it came from, its plug (relink) state, the users.
			struct TexRow {
				const GltfImportImage* rec = nullptr;
				std::string plugTarget; // plug newPath ("" = no plug)
				bool fileOk = false;
				bool loaded = false;
				int pool = -1;
				std::string users; // "mat.2:baseColorTex, ..."
				int useCount = 0;
			};
			std::vector<TexRow> rows;
			int missingFiles = 0;
			std::map<std::string, std::vector<smvk::cache::AssetPlug>>
				plugCache; // per-source plug rows (read once per frame)
			for (const GltfImportImage& rec : m_importImages) {
				if (IsEmbeddedUri(rec.uri))
					continue; // buffer-embedded: no file to fix
				TexRow r;
				r.rec = &rec;
				// Plug state: the source's .meta texture plugs.
				auto pit = plugCache.find(rec.source);
				if (pit == plugCache.end())
					pit = plugCache.emplace(rec.source,
						TexturePlugsOf(rec.source)).first;
				for (const smvk::cache::AssetPlug& p : pit->second)
					if (p.oldUri == rec.uri) {
						r.plugTarget = p.newPath;
						break;
					}
				// File health: the plug target when plugged, else the
				// authored uri's expected path.
				r.fileOk = PathExists(r.plugTarget.empty()
					? expectedFor(rec.uri) : r.plugTarget);
				if (!r.fileOk)
					++missingFiles;
				if (cl) {
					for (const auto& [u, th] : cl->ListTextureUris()) {
						const std::string s = u.Str();
						if (s == rec.identity || BaseName(s) == BaseName(rec.uri)) {
							r.loaded = true;
							r.pool = int(th.index());
							break;
						}
					}
				}
				for (const GltfImportImage::User& u : rec.users) {
					if (!r.users.empty())
						r.users += ", ";
					r.users += u.materialUri + ":" + u.slot;
					++r.useCount;
				}
				if (missingOnly && r.fileOk && (r.loaded || !r.plugTarget.empty()))
					continue;
				rows.push_back(std::move(r));
			}
			ImGui::TextDisabled(
				"%zu image(s), %d missing file(s) — relink saves an input "
				"plug on the source asset's .meta (applied at every "
				"import/cook/bake); 'To expected' copies the file instead.",
				rows.size(), missingFiles);

			// Bulk tools: search a folder for the missing basenames.
			ImGui::SetNextItemWidth(340.0f);
			ImGui::InputTextWithHint("##pathsearch", "search folder...",
				m_pathSearchDir, sizeof(m_pathSearchDir));
			ImGui::SameLine();
			if (ImGui::SmallButton("Scene folder")) {
				std::snprintf(m_pathSearchDir, sizeof(m_pathSearchDir),
					"%s", gltfDir.c_str());
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Find missing")) {
				std::error_code ec;
				std::map<std::string, std::string> byBase;
				size_t scanned = 0;
				if (m_pathSearchDir[0] != '\0' &&
					fs::is_directory(m_pathSearchDir, ec)) {
					for (auto it = fs::recursive_directory_iterator(
							 m_pathSearchDir, ec);
						!ec && it != fs::recursive_directory_iterator();
						it.increment(ec)) {
						if (++scanned > 20000)
							break;
						if (!it->is_regular_file(ec))
							continue;
						byBase.try_emplace(
							it->path().filename().string(),
							it->path().generic_string());
					}
				}
				int found = 0;
				for (const TexRow& r : rows) {
					if (r.fileOk)
						continue;
					auto f = byBase.find(BaseName(r.rec->uri));
					if (f == byBase.end())
						continue;
					RelinkTextureLive(*r.rec, f->second);
					++found;
				}
				Log("Path editor: auto-find in '" +
					std::string(m_pathSearchDir) + "': relinked " +
					std::to_string(found) + " image(s).");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Scans the folder for missing image basenames and "
					"relinks what it finds (live slots + a persistent "
					"input plug on the source asset).");

			int pendingRelink = -1;
			int pendingCopy = -1;
			int pendingForget = -1;
			if (ImGui::BeginTable("##paths_tex", 5,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
						ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
					ImVec2(0, 220))) {
				ImGui::TableSetupColumn("Image URI");
				ImGui::TableSetupColumn("Status",
					ImGuiTableColumnFlags_WidthFixed, 170.0f);
				ImGui::TableSetupColumn("Used by");
				ImGui::TableSetupColumn("Loaded",
					ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Actions",
					ImGuiTableColumnFlags_WidthFixed, 300.0f);
				ImGui::TableHeadersRow();
				int rowId = 0;
				for (const TexRow& r : rows) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(rowId++);
					ImGui::Text("%s", r.rec->uri.c_str());
					ImGui::TableSetColumnIndex(1);
					if (!r.plugTarget.empty()) {
						ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
							"plugged");
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip("%s", r.plugTarget.c_str());
					}
					else if (!r.fileOk)
						ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
							"missing file");
					else if (!r.loaded)
						ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
							"file ok, unloaded");
					else
						ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
							"ok");
					ImGui::TableSetColumnIndex(2);
					ImGui::TextDisabled("%s",
						r.users.empty() ? "(unused)" : r.users.c_str());
					ImGui::TableSetColumnIndex(3);
					ImGui::TextDisabled(
						"%s", r.loaded
							? ("pool " + std::to_string(r.pool)).c_str()
							: "-");
					ImGui::TableSetColumnIndex(4);
					if (ImGui::SmallButton("Relink..."))
						pendingRelink = rowId - 1;
					ImGui::SameLine();
					if (ImGui::SmallButton("To expected"))
						pendingCopy = rowId - 1;
					ImGui::SameLine();
					if (!r.plugTarget.empty()) {
						if (ImGui::SmallButton("Forget plug"))
							pendingForget = rowId - 1;
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(
								"Drops the input plug — future imports "
								"read the authored uri again.");
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (pendingRelink >= 0) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty()
					&& size_t(pendingRelink) < rows.size())
					RelinkTextureLive(*rows[size_t(pendingRelink)].rec, picked);
			}
			if (pendingForget >= 0 && size_t(pendingForget) < rows.size()) {
				const TexRow& r = rows[size_t(pendingForget)];
				ForgetTexturePlug(r.rec->source, r.rec->uri);
			}
			if (pendingCopy >= 0) {
				if (size_t(pendingCopy) >= rows.size()) {
					// (unreachable — kept for symmetry with the pickers)
				}
				else {
					const TexRow& r = rows[size_t(pendingCopy)];
					// Copy the plug target (or a picked file) to the
					// uri's expected path — the permanent, source-visible
					// fix (no editor state involved).
					const std::string expected = expectedFor(r.rec->uri);
					if (expected.empty())
						Log("Path editor: embedded uri — nothing to copy.");
					else {
						std::string src = r.plugTarget;
						if (src.empty() || !PathExists(src)) {
							if (!NativeOpenFile(src, true))
								src.clear();
						}
						if (!src.empty() && PathExists(src)) {
							std::error_code ec;
							fs::create_directories(
								fs::path(expected).parent_path(), ec);
							fs::copy_file(src, expected,
								fs::copy_options::overwrite_existing, ec);
							if (!ec)
								Log("Path editor: copied '" + src + "' -> '"
									+ expected + "' (cook-safe).");
							else
								Log("Path editor: copy failed: " + ec.message());
						}
					}
				}
			}
		}
	}

	// ---- editor images -------------------------------------------------
	if (ImGui::CollapsingHeader("Images (editor file nodes)")) {
		if (m_images.empty()) {
			ImGui::TextDisabled("None (Create > Image... adds one).");
		}
		else {
			int pendingBrowse = -1;
			for (size_t i = 0; i < m_images.size(); ++i) {
				ImGui::PushID(int(i));
				const bool ok = PathExists(m_images[i].path);
				if (missingOnly && ok) {
					ImGui::PopID();
					continue;
				}
				if (!ok)
					ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
						"missing: %s", m_images[i].name.c_str());
				else
					ImGui::TextDisabled("%s", m_images[i].name.c_str());
				ImGui::SameLine();
				ImGui::TextDisabled("%s", m_images[i].path.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Browse..."))
					pendingBrowse = int(i);
				ImGui::PopID();
				if (pendingBrowse >= 0)
					break;
			}
			if (pendingBrowse >= 0 &&
				size_t(pendingBrowse) < m_images.size()) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty() &&
					picked != m_images[size_t(pendingBrowse)].path) {
					m_images[size_t(pendingBrowse)].path = picked;
					AutosaveTour();
					Log("Path editor: image repathed: '" + picked + "'.");
				}
			}
		}
	}

	// ---- outputs -------------------------------------------------------
	if (ImGui::CollapsingHeader("Outputs")) {
		ImGui::BulletText("cook out: %s", m_cook.outPak.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Browse...##cookout")) {
			std::string picked;
			if (NativeSaveFile(picked) && !picked.empty()) {
				m_cook.outPak = picked;
				AutosaveTour();
			}
		}
		ImGui::BulletText("content dir: %s", m_cook.contentDir.c_str());
		ImGui::BulletText("atlas: %s [%s]", m_sglmPath.c_str(),
			m_sglmStatus.c_str());
		ImGui::BulletText("skybox: %s",
			CurrentEnv().skyboxUri.empty() ? "(none)"
											: CurrentEnv().skyboxUri.c_str());
	}
	ImGui::End();
}

} // namespace editor
