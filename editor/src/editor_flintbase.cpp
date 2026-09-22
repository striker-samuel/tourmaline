// Flintbase panel: browse items/products over the HTTP API, resolve
// //flintbase/ paths, open product scenes, and publish cooked paks as
// product versions (spawns `python -m flintbase.publish`, mirroring the
// editor's spawn-PakCooker philosophy: the CLI owns config, storage
// layout, hashing and the DB lifecycle).

#include "editor_app.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include <imgui.h>

namespace editor {

namespace {

void CopyToClipboard(const std::string& text) {
	ImGui::SetClipboardText(text.c_str());
}

} // namespace

void EditorApp::DrawFlintbasePanel() {
	if (!m_showFlintbasePanel)
		return;
	ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Flintbase", &m_showFlintbasePanel)) {
		ImGui::End();
		return;
	}

	smvk::FlintbaseClient client;
	smvk::FlintbaseError err;

	ImGui::TextUnformatted("API:");
	ImGui::SameLine();
	ImGui::TextUnformatted(smvk::FlintbaseClient::DefaultApiUrl().c_str());

	// ---- project + items ------------------------------------------------
	ImGui::InputText("project", m_fbProject, sizeof(m_fbProject));
	ImGui::SameLine();
	if (ImGui::Button("List items")) {
		m_fbItems.clear();
		m_fbItemSel = -1;
		m_fbProducts.clear();
		m_fbProductSel = -1;
		std::vector<smvk::FlintbaseItemInfo> items;
		if (client.ListItems(m_fbProject, items, err)) {
			for (auto& it : items)
				m_fbItems.push_back({it.name, it.type});
			Log("Flintbase: " + std::to_string(items.size()) + " item(s) in '"
				+ m_fbProject + "'");
		}
		else {
			Log("Flintbase: items failed: " + err.message);
		}
	}

	if (!m_fbItems.empty()) {
		if (ImGui::BeginListBox("##items", ImVec2(-1, 120))) {
			for (int i = 0; i < (int)m_fbItems.size(); ++i) {
				const auto& row = m_fbItems[i];
				const bool sel = (i == m_fbItemSel);
				if (ImGui::Selectable((row.name + "  [" + row.type + "]").c_str(), sel)) {
					m_fbItemSel = i;
					std::snprintf(m_fbItem, sizeof(m_fbItem), "%s", row.name.c_str());
					m_fbProducts.clear();
					m_fbProductSel = -1;
				}
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndListBox();
		}
		ImGui::SameLine();
		if (ImGui::Button("List products")) {
			m_fbProducts.clear();
			m_fbProductSel = -1;
			if (client.ListProducts(m_fbProject, m_fbItem, m_fbProducts, err)) {
				Log("Flintbase: " + std::to_string(m_fbProducts.size())
					+ " product(s) for '" + m_fbItem + "'");
			}
			else {
				Log("Flintbase: products failed: " + err.message);
			}
		}
	}

	// ---- products -------------------------------------------------------
	if (!m_fbProducts.empty()) {
		if (ImGui::BeginListBox("##products", ImVec2(-1, 140))) {
			for (int i = 0; i < (int)m_fbProducts.size(); ++i) {
				const auto& p = m_fbProducts[i];
				char label[256];
				std::snprintf(label, sizeof(label), "%-14s %-12s %-8s v%03d [%s]",
					p.name.c_str(),
					(p.task + (p.step.empty() ? "" : "." + p.step)).c_str(),
					(p.latestVersion ? std::to_string(p.latestVersion).c_str() : "-"),
					p.latestVersion,
					p.latestStatus.empty() ? "-" : p.latestStatus.c_str());
				const bool sel = (i == m_fbProductSel);
				if (ImGui::Selectable(label, sel)) {
					m_fbProductSel = i;
					std::snprintf(m_fbTarget, sizeof(m_fbTarget), "%s/latest",
						p.Path().c_str());
				}
				if (sel)
					ImGui::SetItemDefaultFocus();
			}
			ImGui::EndListBox();
		}
	}

	// ---- target path + resolve/open --------------------------------------
	ImGui::Separator();
	ImGui::InputText("path", m_fbTarget, sizeof(m_fbTarget));
	if (ImGui::Button("Resolve")) {
		smvk::FlintbaseResolved r;
		if (client.Resolve(std::string(smvk::vfs::FlintbaseRemainder(
				smvk::vfs::Normalize(m_fbTarget))), r, err)) {
			Log("Flintbase: " + r.resolvedPath + " (" + std::to_string(r.files.size())
				+ " file(s), manifest " + r.contentsHash.substr(0, 12) + "...)");
			for (const auto& f : r.files)
				Log("  " + f.path + "  " + std::to_string(f.size) + "B  "
					+ f.sha256.substr(0, 12) + "...");
			CopyToClipboard(r.resolvedPath);
		}
		else {
			Log("Flintbase: resolve failed: " + err.message);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button("Open scene")) {
		OpenScene(m_fbTarget);
	}
	ImGui::SameLine();
	if (ImGui::Button("Copy path")) {
		CopyToClipboard(m_fbTarget);
	}

	// ---- publish --------------------------------------------------------
	ImGui::Separator();
	ImGui::TextUnformatted("Publish (cook out-pak -> product version)");
	if (m_fbPublishSource[0] == '\0' && !m_cook.outPak.empty())
		std::snprintf(m_fbPublishSource, sizeof(m_fbPublishSource), "%s",
			m_cook.outPak.c_str());
	ImGui::InputText("source file", m_fbPublishSource, sizeof(m_fbPublishSource));
	if (m_publishRunning) {
		ImGui::TextUnformatted("publishing...");
	}
	else if (ImGui::Button("Publish")) {
		PublishToFlintbase();
	}

	ImGui::End();
}

void EditorApp::PublishToFlintbase() {
	if (m_publishRunning || m_fbPublishSource[0] == '\0' || m_fbTarget[0] == '\0') {
		Log("Flintbase publish: needs a source file and a target path.");
		return;
	}
	const std::string source = m_fbPublishSource;
	// Product-level path only: strip a trailing /latest or /vNNN the
	// browser may have filled in (the publisher assigns the version).
	std::string target = m_fbTarget;
	{
		const size_t slash = target.rfind('/');
		if (slash != std::string::npos) {
			const std::string last = target.substr(slash + 1);
			if (last == "latest" || (last.size() > 1 && last[0] == 'v'
				&& last.find_first_not_of("0123456789", 1) == std::string::npos))
				target = target.substr(0, slash);
		}
	}
	m_publishRunning = true;
	Log("Flintbase publish: " + source + " -> " + target);
	m_publishFuture = std::async(std::launch::async,
		[source, target]() -> PublishResult {
			PublishResult r{};
			std::ostringstream log;
			// python executable: FLINTBASE_PYTHON env override, else "python".
			const char* py = std::getenv("FLINTBASE_PYTHON");
			std::ostringstream cmd;
			cmd << "\"" << (py && *py ? py : "python") << "\""
				<< " -m flintbase.publish"
				<< " \"" << source << "\""
				<< " \"" << target << "\""
				<< " 2>&1";
			log << "publish: " << cmd.str() << "\n";
#ifdef _WIN32
			FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
			FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
			if (!pipe) {
				r.log = log.str() + "publish: failed to spawn python\n";
				return r;
			}
			std::array<char, 512> buf{};
			while (std::fgets(buf.data(), int(buf.size()), pipe))
				log << buf.data();
#ifdef _WIN32
			const int rc = _pclose(pipe);
#else
			const int rc = pclose(pipe);
#endif
			log << "publish: exit=" << rc << "\n";
			r.ok = (rc == 0);
			r.log = log.str();
			return r;
		});
}

void EditorApp::PollPublishProcess() {
	if (!m_publishRunning || !m_publishFuture.valid())
		return;
	if (m_publishFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return;
	PublishResult r = m_publishFuture.get();
	m_publishRunning = false;
	std::istringstream ss(r.log);
	std::string line;
	while (std::getline(ss, line))
		Log(line);
	Log(r.ok ? "Flintbase publish finished OK."
		: "Flintbase publish FAILED (python -m flintbase.publish; needs the"
			" flintbase package importable - set FLINTBASE_PYTHON or"
			" PYTHONPATH/DB env in the editor environment).");
}

} // namespace editor
