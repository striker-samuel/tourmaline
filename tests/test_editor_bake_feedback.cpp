// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT
#include "../editor/src/editor_bake_log.h"
#include "../editor/src/editor_bake_progress.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main() {
	using editor::ParseBakeProgress;
	auto p = ParseBakeProgress("lightmap bake: 3/12 chunks (25%), 42 texels");
	assert(p.recognized && p.stage.find("3/12") != std::string::npos);
	assert(p.fraction == 0.25f);
	p = ParseBakeProgress("lightmap bake: 99/12 chunks (bad upstream count)");
	assert(p.recognized && p.fraction == 1.0f);
	p = ParseBakeProgress("lightmap bake: 0/0 chunks");
	assert(!p.recognized);
	p = ParseBakeProgress("lightmap bake: 4/x chunks");
	assert(!p.recognized);
	p = ParseBakeProgress("lightmap bake: 12 chunks, 100 texels, 64 rays/texel");
	assert(p.recognized && p.fraction == 0.0f);
	p = ParseBakeProgress("lightmap bake: done: 12 chunks");
	assert(p.recognized && p.fraction == 1.0f);
	p = ParseBakeProgress("givolume bake: 256/1024 probes");
	assert(p.recognized && p.fraction == 0.25f);
	p = ParseBakeProgress("givolume bake: done: 1024 probes");
	assert(p.recognized && p.fraction == 1.0f);
	p = ParseBakeProgress("  [LM] xatlas density converged at 3.0");
	assert(p.recognized && p.fraction < 0.0f);
	p = ParseBakeProgress("  [GIV] volume 'Kitchen': priority=1");
	assert(p.recognized && p.stage.find("Kitchen") != std::string::npos);
	assert(!ParseBakeProgress("ordinary cook output").recognized);

	const auto root = std::filesystem::temp_directory_path() /
		("tourmaline-bake-log-test-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	std::string path;
	{
		editor::BakeLogFile log(root.string(), "lightmap-test");
		path = log.Path();
		assert(!path.empty());
		log.Write("first progress line");
		log.Write("failure detail", true);
		// A reader can follow the file while the bake is still running.
		std::ifstream in(path);
		const std::string contents((std::istreambuf_iterator<char>(in)),
			std::istreambuf_iterator<char>());
		assert(contents == "first progress line\n[stderr] failure detail\n");
	}
	std::filesystem::remove_all(root);
}
