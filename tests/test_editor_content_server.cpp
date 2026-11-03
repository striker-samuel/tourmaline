// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
// CPU-only content-server routing and live watcher regression tests.
#include "editor_content_server.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Event = smvk::FileWatcher::Event;
using Route = editor::WatchRoute;
using Server = editor::EditorContentServer;
int failures = 0;
void Check(bool ok, const char* label) {
	std::cout << (ok ? "PASS " : "FAIL ") << label << '\n';
	if (!ok) ++failures;
}
void Write(const fs::path& path, const char* bytes) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	out << bytes;
}
bool Await(Server& server, const fs::path& expected, Route route) {
	const auto until = std::chrono::steady_clock::now()
		+ std::chrono::seconds(4);
	while (std::chrono::steady_clock::now() < until) {
		const auto batch = server.Poll(50);
		for (const auto& change : batch.changes)
			if (change.path == expected.lexically_normal().generic_string()
					&& change.route == route)
				return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(30));
	}
	return false;
}
void TestRouting(const fs::path& root) {
	const fs::path image = root / "content" / "brick.PNG";
	Write(root / "content" / "brick.texture", "texture");
	const std::vector<smvk::FileWatcher::Change> raw = {
		{ (root / "content" / "scene.GLB").generic_string(), Event::Added },
		{ (root / "content" / "scene.GLB").generic_string(), Event::Modified },
		{ (root / "content" / "scene.gltf.meta").generic_string(), Event::Modified },
		{ (root / "content" / "scene.tour").generic_string(), Event::Modified },
		{ image.generic_string(), Event::Modified },
		{ (root / "content" / "missing.png").generic_string(), Event::Removed }
	};
	const auto changes = Server::RouteChanges(raw);
	Check(changes.size() == 5, "duplicate paths coalesced");
	auto Has = [&](const fs::path& p, Route r, Event e) {
		for (const auto& change : changes)
			if (change.path == p.generic_string()
					&& change.route == r && change.event == e)
				return true;
		return false;
	};
	Check(Has(root / "content" / "scene.GLB",
		Route::GltfSource, Event::Modified), "mesh source / latest event");
	Check(Has(root / "content" / "scene.gltf.meta",
		Route::MetaSidecar, Event::Modified), "identity sidecar");
	Check(Has(root / "content" / "scene.tour",
		Route::TourDoc, Event::Modified), "tour routed, not auto-replayed");
	Check(Has(image, Route::TextureSource, Event::Modified),
		"image beside texture asset");
	Check(Has(root / "content" / "missing.png",
		Route::Removed, Event::Removed), "removed route wins");
}
void TestLive(const fs::path& root) {
	const fs::path source = root / "content" / "models" / "fresh.gltf";
	std::error_code ec;
	fs::create_directories(source.parent_path(), ec);
	Server server;
	std::string err;
	Check(server.Start(root.generic_string(), err), "start content server");
	Check(server.Running(), "watcher running");
	// The native watcher thread may still be arming recursive roots.
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	Write(source, "{}");
	Check(Await(server, source, Route::GltfSource),
		"new source delivered as typed change");
	server.Stop();
	Check(!server.Running(), "stop releases watcher");
	Check(server.Poll().changes.empty(), "stopped server is silent");
	Check(!server.Start("", err), "empty project root rejected");
	Check(!server.Running(), "failed start remains stopped");
}
} // namespace

int main() {
	const fs::path root = fs::temp_directory_path()
		/ ("tourmaline_editor_content_server_"
			+ std::to_string(std::chrono::steady_clock::now()
				.time_since_epoch().count()));
	std::error_code ec;
	fs::create_directories(root, ec);
	TestRouting(root);
	TestLive(root);
	fs::remove_all(root, ec);
	return failures ? 1 : 0;
}
