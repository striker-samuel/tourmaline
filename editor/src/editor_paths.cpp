// Path editor: consolidated view over every file/path the scene depends
// on (File > Path editor). Solves missing paths and easy repaths —
// notably broken/missing texture links when bringing a glTF whose image
// URIs no longer resolve.
//
// Sections: scene source + tour, scene references, glTF image URIs
// (the texture core), external .bin buffers, editor image definitions,
// and outputs (cook pak, lightmap atlas, env skybox).
//
// Two fix flavors for textures:
//   - Live relink (undoable, autosaved): loads the replacement KTX2 and
//     reassigns every material slot that wants the image. Persists as a
//     path.remap.* editorSetting (editor-only, never cooked) and
//     re-applies after every import, so the viewport stays fixed.
//   - Copy to expected path (permanent, cook-safe): places the file
//     where the glTF URI points, fixing reloads, reimports and cooks.

#include "editor_app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>

#include <imgui.h>

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

// glTF material index -> live world material index (-1 when unmapped).
// The importer stamps "<base>#mat.N" URIs in creation order, so the
// suffix identifies the glTF material exactly.
int EditorApp::GltfMatToWorld(int gltfMat) const {
	if (!m_world)
		return -1;
	const std::string want = "#mat." + std::to_string(gltfMat);
	for (uint32_t i = 0; i < uint32_t(m_world->materialLocations.size()); ++i) {
		const auto& loc = m_world->materialLocations[i];
		if (!loc.cluster || !loc.handle.valid())
			continue;
		const std::string uri =
			std::string(loc.cluster->GetMaterialUri(loc.handle).Str());
		if (uri.size() >= want.size() &&
			uri.compare(uri.size() - want.size(), want.size(), want) == 0)
			return int(i);
	}
	return -1;
}

void EditorApp::SavePathRemapsToDoc(smvk::tour::Doc& doc) const {
	for (size_t i = 0; i < m_pathRemaps.size(); ++i) {
		const std::string base = "path.remap." + std::to_string(i);
		doc.editorSettings.emplace_back(base + ".old", m_pathRemaps[i].oldUri);
		doc.editorSettings.emplace_back(base + ".new", m_pathRemaps[i].newPath);
	}
}

void EditorApp::LoadPathRemapsFromDoc(const smvk::tour::Doc& doc) {
	std::map<size_t, PathRemap> defs;
	for (const auto& [key, val] : doc.editorSettings) {
		if (key.rfind("path.remap.", 0) != 0)
			continue;
		const size_t idEnd = key.find('.', 11);
		if (idEnd == std::string::npos)
			continue;
		size_t idx = 0;
		try {
			idx = std::stoull(key.substr(11, idEnd - 11));
		}
		catch (...) {
			continue;
		}
		const std::string attr = key.substr(idEnd + 1);
		if (attr == "old")
			defs[idx].oldUri = val;
		else if (attr == "new")
			defs[idx].newPath = val;
	}
	m_pathRemaps.clear();
	for (auto& [idx, r] : defs) {
		(void)idx;
		if (!r.oldUri.empty() && !r.newPath.empty())
			m_pathRemaps.push_back(r);
	}
}

// Silent post-load fixup (no history entries — import repair): re-mints
// each remapped file and fills material slots that are still missing.
void EditorApp::ApplyPathRemaps() {
	if (!m_world || m_pathRemaps.empty() || !m_hasGltfModel)
		return;
	smvk::Cluster* cl = m_world->clusters.empty()
		? nullptr
		: m_world->clusters[0].get();
	if (!cl)
		return;
	static const char* kAttrs[5] = { "baseColorTex", "metallicRoughTex",
		"normalTex", "occlusionTex", "emissiveTex" };
	for (const PathRemap& r : m_pathRemaps) {
		if (!PathExists(r.newPath)) {
			Log("Path editor: remap target gone: '" + r.newPath + "'.");
			continue;
		}
		// Reuse an already-registered texture for the file, else load it.
		int pool = -1;
		for (const auto& [uri, th] : cl->ListTextureUris()) {
			const std::string u = uri.Str();
			if (u == r.newPath || BaseName(u) == BaseName(r.newPath)) {
				pool = int(th.index());
				break;
			}
		}
		if (pool < 0)
			pool = AddTextureFromFile(r.newPath);
		if (pool < 0)
			continue;
		int filled = 0;
		for (size_t mi = 0; mi < m_gltfModel.materials.size(); ++mi) {
			const auto& gm = m_gltfModel.materials[mi];
			int texIdx[5] = { gm.pbrMetallicRoughness.baseColorTexture.index,
				gm.pbrMetallicRoughness.metallicRoughnessTexture.index,
				gm.normalTexture.index, gm.occlusionTexture.index,
				gm.emissiveTexture.index };
			for (int s = 0; s < 5; ++s) {
				if (texIdx[s] < 0 ||
					texIdx[s] >= int(m_gltfModel.textures.size()))
					continue;
				const int img = m_gltfModel.textures[size_t(texIdx[s])].source;
				if (img < 0 || img >= int(m_gltfModel.images.size()))
					continue;
				if (m_gltfModel.images[size_t(img)].uri != r.oldUri)
					continue;
				const int mw = GltfMatToWorld(int(mi));
				if (mw < 0)
					continue;
				glm::vec4 v4;
				double num = 0.0;
				ReadSharedMatAttr(uint32_t(mw), kAttrs[s], v4, num);
				if (int(num) == pool)
					continue;
				if (int(num) >= 0)
					continue; // user-bound slot: never steal it
				WriteSharedMatAttr(uint32_t(mw), kAttrs[s], v4,
					double(pool));
				++filled;
			}
		}
		if (filled > 0)
			Log("Path editor: remap applied '" + r.oldUri + "' -> '" +
				r.newPath + "' (" + std::to_string(filled) + " slot(s)).");
	}
}

// Interactive relink of one glTF image: loads newPath (or reuses the
// registered texture for it), reassigns every material slot that wants
// imageIndex via undoable commands, records the remap and autosaves.
int EditorApp::RelinkTextureLive(int imageIndex, const std::string& newPath) {
	if (!m_world || !m_hasGltfModel || imageIndex < 0 ||
		imageIndex >= int(m_gltfModel.images.size()) || newPath.empty())
		return -1;
	smvk::Cluster* cl = m_world->clusters.empty()
		? nullptr
		: m_world->clusters[0].get();
	if (!cl)
		return -1;
	const std::string oldUri = m_gltfModel.images[size_t(imageIndex)].uri;
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
	static const char* kAttrs[5] = { "baseColorTex", "metallicRoughTex",
		"normalTex", "occlusionTex", "emissiveTex" };
	int touched = 0;
	for (size_t mi = 0; mi < m_gltfModel.materials.size(); ++mi) {
		const auto& gm = m_gltfModel.materials[mi];
		int texIdx[5] = { gm.pbrMetallicRoughness.baseColorTexture.index,
			gm.pbrMetallicRoughness.metallicRoughnessTexture.index,
			gm.normalTexture.index, gm.occlusionTexture.index,
			gm.emissiveTexture.index };
		for (int s = 0; s < 5; ++s) {
			if (texIdx[s] < 0 ||
				texIdx[s] >= int(m_gltfModel.textures.size()))
				continue;
			const int img = m_gltfModel.textures[size_t(texIdx[s])].source;
			if (img != imageIndex)
				continue;
			const int mw = GltfMatToWorld(int(mi));
			if (mw < 0)
				continue;
			glm::vec4 v4;
			double num = 0.0;
			ReadSharedMatAttr(uint32_t(mw), kAttrs[s], v4, num);
			if (int(num) == pool)
				continue;
			EditCmd c{};
			c.verb = EditVerb::SetMatAttr;
			c.matWorld = uint32_t(mw);
			c.attr = kAttrs[s];
			c.beforeVec4 = v4;
			c.beforeNum = num;
			c.vec4 = v4;
			c.number = double(pool);
			Do(c);
			++touched;
		}
	}
	// Record the remap (one entry per old URI — newest wins).
	bool known = false;
	for (PathRemap& r : m_pathRemaps)
		if (r.oldUri == oldUri) {
			r.newPath = newPath;
			known = true;
		}
	if (!known)
		m_pathRemaps.push_back({ oldUri, newPath });
	AutosaveTour();
	ClearTexPreview();
	Log("Path editor: relinked '" + oldUri + "' -> '" + newPath +
		"' (" + std::to_string(touched) + " slot(s), pool " +
		std::to_string(pool) + ").");
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
		ImGui::TextDisabled("No scene. File > Open file... (.gltf/.pak/.tour)");
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

	// ---- textures (the glTF core) ------------------------------------
	if (ImGui::CollapsingHeader("Textures (glTF images)",
			ImGuiTreeNodeFlags_DefaultOpen)) {
		if (!m_hasGltfModel) {
			ImGui::TextDisabled(
				"Pak scene: no glTF image list (textures live in the pak).");
		}
		else if (m_gltfModel.images.empty()) {
			ImGui::TextDisabled("No images in the glTF.");
		}
		else {
			smvk::Cluster* cl = m_world->clusters.empty()
				? nullptr
				: m_world->clusters[0].get();
			struct TexRow {
				int img = -1;
				std::string uri;
				std::string expected;
				bool fileOk = false;
				bool loaded = false;
				int pool = -1;
				std::string users; // "mat.2:baseColorTex, ..."
				int useCount = 0;
			};
			std::vector<TexRow> rows;
			int missingFiles = 0;
			for (size_t ii = 0; ii < m_gltfModel.images.size(); ++ii) {
				TexRow r;
				r.img = int(ii);
				r.uri = m_gltfModel.images[ii].uri;
				if (IsEmbeddedUri(r.uri))
					continue; // buffer-embedded: no file to fix
				r.expected = expectedFor(r.uri);
				r.fileOk = PathExists(r.expected);
				if (!r.fileOk)
					++missingFiles;
				if (cl) {
					for (const auto& [u, th] : cl->ListTextureUris()) {
						const std::string s = u.Str();
						if (s == r.expected || s == r.uri ||
							BaseName(s) == BaseName(r.uri)) {
							r.loaded = true;
							r.pool = int(th.index());
							break;
						}
					}
				}
				// Which (material, slot) pairs want this image?
				static const char* kSlot[5] = { "baseColorTex",
					"metallicRoughTex", "normalTex", "occlusionTex",
					"emissiveTex" };
				for (size_t mi = 0; mi < m_gltfModel.materials.size();
					++mi) {
					const auto& gm = m_gltfModel.materials[mi];
					int ti[5] = {
						gm.pbrMetallicRoughness.baseColorTexture.index,
						gm.pbrMetallicRoughness.metallicRoughnessTexture
							.index,
						gm.normalTexture.index, gm.occlusionTexture.index,
						gm.emissiveTexture.index };
					for (int s = 0; s < 5; ++s) {
						if (ti[s] < 0 ||
							ti[s] >= int(m_gltfModel.textures.size()))
							continue;
						if (m_gltfModel.textures[size_t(ti[s])].source !=
							int(ii))
							continue;
						if (!r.users.empty())
							r.users += ", ";
						r.users += "mat." + std::to_string(mi) + ":" +
							kSlot[s];
						++r.useCount;
					}
				}
				if (missingOnly && r.fileOk && r.loaded)
					continue;
				rows.push_back(std::move(r));
			}
			ImGui::TextDisabled(
				"%zu image(s), %d missing file(s) — relink fixes the "
				"viewport (editor-only); copy-to-expected also fixes cooks.",
				m_gltfModel.images.size(), missingFiles);

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
					auto f = byBase.find(BaseName(r.uri));
					if (f == byBase.end())
						continue;
					RelinkTextureLive(r.img, f->second);
					++found;
				}
				Log("Path editor: auto-find in '" +
					std::string(m_pathSearchDir) + "': relinked " +
					std::to_string(found) + " image(s).");
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(
					"Scans the folder for missing image basenames and "
					"live-relinks what it finds (undoable, editor-only).");

			int pendingRelink = -1;
			int pendingCopy = -1;
			if (ImGui::BeginTable("##paths_tex", 5,
					ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
						ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY,
					ImVec2(0, 220))) {
				ImGui::TableSetupColumn("Image URI");
				ImGui::TableSetupColumn("Status",
					ImGuiTableColumnFlags_WidthFixed, 150.0f);
				ImGui::TableSetupColumn("Used by");
				ImGui::TableSetupColumn("Loaded",
					ImGuiTableColumnFlags_WidthFixed, 80.0f);
				ImGui::TableSetupColumn("Actions",
					ImGuiTableColumnFlags_WidthFixed, 210.0f);
				ImGui::TableHeadersRow();
				for (const TexRow& r : rows) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(r.img);
					ImGui::Text("%s", r.uri.c_str());
					ImGui::TableSetColumnIndex(1);
					if (!r.fileOk)
						ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
							"missing file");
					else if (!r.loaded)
						ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
							"file ok, unloaded");
					else
						ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.0f),
							"ok");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip("%s", r.expected.c_str());
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
						pendingRelink = r.img;
					ImGui::SameLine();
					if (ImGui::SmallButton("To expected"))
						pendingCopy = r.img;
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(
							"Copies the relinked file to the expected "
							"path (permanent, cook-safe).");
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
			if (pendingRelink >= 0) {
				std::string picked;
				if (NativeOpenFile(picked, true) && !picked.empty())
					RelinkTextureLive(pendingRelink, picked);
			}
			if (pendingCopy >= 0) {
				const TexRow* hit = nullptr;
				for (const TexRow& r : rows)
					if (r.img == pendingCopy) {
						hit = &r;
						break;
					}
				if (hit && !hit->expected.empty()) {
					std::string src;
					for (const PathRemap& pr : m_pathRemaps)
						if (pr.oldUri == hit->uri) {
							src = pr.newPath;
							break;
						}
					if (src.empty() || !PathExists(src)) {
						if (!NativeOpenFile(src, true))
							src.clear();
					}
					if (!src.empty() && PathExists(src)) {
						std::error_code ec;
						fs::create_directories(
							fs::path(hit->expected).parent_path(), ec);
						fs::copy_file(src, hit->expected,
							fs::copy_options::overwrite_existing, ec);
						if (!ec)
							Log("Path editor: copied '" + src + "' -> '" +
								hit->expected + "' (cook-safe).");
						else
							Log("Path editor: copy failed: " + ec.message());
					}
				}
			}
			// Active remaps (editor-only; cooks need the copy above).
			if (!m_pathRemaps.empty()) {
				ImGui::Separator();
				ImGui::TextDisabled(
					"%zu live relink(s) — editor-only, re-applied on load "
					"(cooks need 'To expected'):",
					m_pathRemaps.size());
				int dropRemap = -1;
				for (size_t i = 0; i < m_pathRemaps.size(); ++i) {
					ImGui::PushID(int(i));
					ImGui::BulletText("%s -> %s",
						m_pathRemaps[i].oldUri.c_str(),
						m_pathRemaps[i].newPath.c_str());
					ImGui::SameLine();
					if (ImGui::SmallButton("Forget"))
						dropRemap = int(i);
					ImGui::PopID();
				}
				if (dropRemap >= 0) {
					m_pathRemaps.erase(
						m_pathRemaps.begin() + ptrdiff_t(dropRemap));
					AutosaveTour();
				}
			}
		}
	}

	// ---- buffers -----------------------------------------------------
	if (ImGui::CollapsingHeader("Buffers (.bin)")) {
		if (!m_hasGltfModel) {
			ImGui::TextDisabled("Pak scene: no glTF buffer list.");
		}
		else {
			bool any = false;
			for (size_t i = 0; i < m_gltfModel.buffers.size(); ++i) {
				const std::string uri = m_gltfModel.buffers[i].uri;
				if (IsEmbeddedUri(uri))
					continue;
				any = true;
				const std::string exp = expectedFor(uri);
				const bool ok = PathExists(exp);
				if (missingOnly && ok)
					continue;
				if (!ok)
					ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
						"missing: %s", uri.c_str());
				else
					ImGui::TextDisabled("ok: %s", uri.c_str());
			}
			if (!any)
				ImGui::TextDisabled("All buffers embedded (no files).");
			else
				ImGui::TextDisabled(
					"Buffers load at import — a missing .bin needs the "
					"file restored next to the glTF, then reopen.");
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
