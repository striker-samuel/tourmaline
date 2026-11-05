// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Blocking modal task window: the single progress surface for the long
// subprocesses (CookToPak, BakeGI). Workers push one
// line per output line into their mutex-guarded feed; the Poll* pumps
// (main thread) forward each line here AND to the Console (the persistent
// record) so the popup details live progress while the modal blocks the
// rest of the UI. Result stays up until acknowledged (Close) — a failed
// bake/cook can no longer scroll past unseen.

#include "editor_app.h"
#include "ui/dialogs/system_modal.h"
#include <algorithm>

#include <imgui.h>

namespace editor {

namespace {

// Detail-log cap (tail kept; head drops — workers can emit thousands of
// lines on big cooks and the popup must stay cheap to draw).
inline constexpr size_t kTaskLogCap = 500;

} // namespace

void EditorApp::TaskWindowBegin(const std::string& title,
	const std::string& stage) {
	m_taskWindow.wantOpen = true;
	m_taskWindow.inBackground = false;
	m_taskWindow.opened = false; // re-arm OpenPopup below
	m_taskWindow.title = title;
	m_taskWindow.stage = stage;
	m_taskWindow.progress = -1.0f;
	m_taskWindow.lines.clear();
	m_taskWindow.done = false;
	m_taskWindow.ok = false;
	m_taskWindow.result.clear();
	m_taskWindow.logPath.clear();
}

void EditorApp::TaskWindowPush(const std::string& line) {
	if (!m_taskWindow.wantOpen)
		return;
	m_taskWindow.lines.push_back(line);
	while (m_taskWindow.lines.size() > kTaskLogCap)
		m_taskWindow.lines.erase(m_taskWindow.lines.begin());
}

void EditorApp::TaskWindowStage(const std::string& stage, float progress) {
	m_taskWindow.stage = stage;
	m_taskWindow.progress = progress;
}

void EditorApp::TaskWindowFinish(bool ok, const std::string& result) {
	m_taskWindow.done = true;
	m_taskWindow.ok = ok;
	m_taskWindow.result = result;
	// A backgrounded task pops back for the result (failures must not
	// finish silently behind the viewport).
	m_taskWindow.inBackground = false;
}

void EditorApp::ReopenTaskWindow() {
	if (!m_taskWindow.wantOpen)
		return;
	m_taskWindow.inBackground = false;
	m_taskWindow.opened = false; // re-arm OpenPopup below
}

void EditorApp::DrawTaskWindow() {
	if (!m_taskWindow.wantOpen || m_taskWindow.inBackground)
		return;
	if (!m_taskWindow.opened) {
		ImGui::OpenPopup(m_taskWindow.popupId.c_str());
		m_taskWindow.opened = true;
	}
	ui::PrepareSystemModal(ImVec2(640, 420));
	if (!ImGui::BeginPopupModal(m_taskWindow.popupId.c_str(), nullptr,
			ui::kSystemModalFlags)) {
		// Popup not up this frame (e.g. stacked behind another modal):
		// stay armed, keep blocking, retry next frame.
		return;
	}
	ImGui::Text("%s", m_taskWindow.title.c_str());
	ImGui::Separator();
	ImGui::Text("%s", m_taskWindow.stage.c_str());
	if (!m_taskWindow.logPath.empty()) {
		ImGui::TextDisabled("Live log: %s", m_taskWindow.logPath.c_str());
		ImGui::SameLine();
		if (ImGui::SmallButton("Copy path"))
			ImGui::SetClipboardText(m_taskWindow.logPath.c_str());
	}
	if (m_taskWindow.progress < 0.0f)
		ImGui::ProgressBar(-1.0f, ImVec2(-1, 0), "");
	else
		ImGui::ProgressBar(m_taskWindow.progress, ImVec2(-1, 0), "");
	// Detail log (auto-follows the tail while running).
	{
		const float footerH = ImGui::GetFrameHeightWithSpacing() + 8.0f;
		ImGui::BeginChild("##tasklog", ImVec2(0, -footerH), true,
			ImGuiWindowFlags_HorizontalScrollbar);
		for (const std::string& line : m_taskWindow.lines)
			ImGui::TextUnformatted(line.c_str());
		if (!m_taskWindow.done && !m_taskWindow.lines.empty())
			ImGui::SetScrollHereY(1.0f);
		ImGui::EndChild();
	}
	if (!m_taskWindow.done) {
		// Running: the background hatch hides the modal without
		// stopping the in-process baker (no safe cancellation API yet).
		if (ImGui::Button("Run in background")) {
			m_taskWindow.inBackground = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		ImGui::TextDisabled("the worker continues; the top-bar button "
			"reopens this window.");
	}
	else {
		// Finished: result + acknowledge. The Console carries the same
		// lines for the record.
		// A long failure reason must never shove the Close button
		// outside the modal. Reserve fixed button space and clip only the
		// one-line summary; full details remain in the log and tooltip.
		const float closeW = 76.0f;
		const float messageW = std::max(24.0f,
			ImGui::GetContentRegionAvail().x - closeW
			- ImGui::GetStyle().ItemSpacing.x);
		ImGui::PushStyleColor(ImGuiCol_Text, m_taskWindow.ok
			? ImVec4(0.4f, 1.0f, 0.5f, 1.0f)
			: ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
		ImGui::BeginChild("##task_result_summary",
			ImVec2(messageW, ImGui::GetFrameHeight()), false,
			ImGuiWindowFlags_NoScrollbar
			| ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::TextUnformatted(m_taskWindow.result.c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", m_taskWindow.result.c_str());
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::SameLine();
		if (ImGui::Button("Close", ImVec2(closeW, 0))) {
			m_taskWindow.wantOpen = false;
			m_taskWindow.opened = false;
			ImGui::CloseCurrentPopup();
		}
	}
	ImGui::EndPopup();
}

} // namespace editor
