// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Background pak preload: file read + whole-image CRC + per-entry LZ4
// decompression, kicked BEFORE the renderer starts Vulkan so it overlaps
// instance/device/swapchain init and shader loading. App-host logic.

#include "PakPreload.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetModuleFileNameA (exe-relative pak resolution)
#else
#include <unistd.h> // readlink (/proc/self/exe)
#endif

namespace app {

std::string ResolvePakOpenPath(const std::string& scenePak) {
	std::string pakOpenPath;
	std::vector<std::string> candidates;
	if (!scenePak.empty()) {
		std::string rel = scenePak;
		while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
			rel.erase(rel.begin());
		candidates.push_back(scenePak);
		if (!rel.empty() && rel != scenePak) {
			candidates.push_back(rel);
			char exePath[1024] = {};
#ifdef _WIN32
			DWORD n = GetModuleFileNameA(nullptr, exePath, DWORD(sizeof(exePath)));
			(void)n;
#else
			const ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
			if (n > 0)
				exePath[n] = '\0';
#endif
			if (exePath[0])
				candidates.push_back(
					(std::filesystem::path(exePath).parent_path() / rel).string());
		}
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!c.empty() && std::filesystem::exists(std::filesystem::path(c), ec)) {
			pakOpenPath = c;
			break;
		}
	}
	if (pakOpenPath.empty() && !scenePak.empty())
		std::cerr << "[App] pak not found: '" << scenePak
			<< "' (tried as-is, CWD-relative and exe-relative)" << std::endl;
	return pakOpenPath;
}

std::future<PakPreload> KickPakPreload(const std::string& pakPath) {
	return std::async(std::launch::async, [pakPath]() {
		const auto t0 = std::chrono::steady_clock::now();
		PakPreload p;
		p.scenePak = pakPath;
		p.openPath = ResolvePakOpenPath(p.scenePak);
		if (p.openPath.empty()) {
			p.error = "pak not found: '" + p.scenePak
				+ "' (tried as-is, CWD-relative and exe-relative)";
			return p;
		}
		std::ifstream pakFile(p.openPath, std::ios::binary | std::ios::ate);
		if (!pakFile) {
			p.error = "pak open failed: '" + p.openPath + "'";
			return p;
		}
		const auto pakEnd = pakFile.tellg();
		if (pakEnd == std::ifstream::pos_type(-1)) {
			p.error = "pak unreadable: '" + p.openPath + "'";
			return p;
		}
		pakFile.seekg(0);
		auto image = std::make_shared<std::vector<uint8_t>>(
			static_cast<size_t>(pakEnd));
		if (!pakFile.read(reinterpret_cast<char*>(image->data()),
				static_cast<std::streamsize>(image->size()))) {
			p.error = "pak read failed: '" + p.openPath + "'";
			return p;
		}
		p.image = std::move(image);
		const auto t1 = std::chrono::steady_clock::now();
		// ONE open covers everything: PakBackend::Open parses + verifies
		// the CRC + (for big images) parallel-decompresses every LZ4
		// entry. Its Reader() serves scene discovery + the cluster load;
		// the old path opened the reader twice (double CRC + double
		// decompress).
		std::string pakErr;
		p.backend = smvk::PakBackend::Open(p.image, p.scenePak, pakErr);
		const auto t2 = std::chrono::steady_clock::now();
		if (!p.backend) {
			p.image.reset();
			p.error = "pak parse failed: " + pakErr;
			return p;
		}
		std::cerr << "[App] pak preloaded (read "
			<< std::chrono::duration<double>(t1 - t0).count() << "s, open "
			<< std::chrono::duration<double>(t2 - t1).count() << "s): "
			<< p.backend->Reader().EntryCount() << " entries, "
			<< p.image->size() / (1024 * 1024) << " MB\n";
		return p;
	});
}

} // namespace app
