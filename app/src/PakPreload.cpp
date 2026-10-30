// Copyright (c) 2026 Samuel Martin
// SPDX-License-Identifier: MIT

// Background pak preload: file read + whole-image CRC (LZ4 entries
// decompress lazily at first use inside the reader), kicked BEFORE the
// renderer starts Vulkan so it overlaps instance/device/swapchain init
// and shader loading. App-host logic.

#include "PakPreload.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <Engine/Profile/profile.h>

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
	if (scenePak.empty())
		return pakOpenPath;
	// EXPLICIT, consistent resolution ladder (the old code only added the
	// exe-relative candidate when the path had LEADING SLASHES to strip —
	// a vfs-style "/content/..." — so an ORDINARY relative path
	// ("content/paks/bistro.pak") never received the intended
	// executable-relative fallback and failed whenever the CWD wasn't
	// the build root):
	//   1. as-is (absolute paths + CWD-relative),
	//   2. CWD-relative with the leading separators stripped (vfs-style),
	//   3. exe-relative (any relative path: ordinary + stripped).
	std::vector<std::string> candidates;
	candidates.push_back(scenePak);
	std::string rel = scenePak;
	while (!rel.empty() && (rel.front() == '/' || rel.front() == '\\'))
		rel.erase(rel.begin());
	if (!rel.empty() && rel != scenePak)
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
	// Exe-relative applies to anything that is not drive/server-ROOTED
	// (has_root_name): ordinary relative paths AND the vfs-style
	// "/content/..." (root-directory paths are relative on Windows, and
	// on POSIX the stripped `rel` is what actually joins — matching the
	// old leading-slash behavior). Only "C:\..." / UNC paths skip it.
	const std::filesystem::path scenePakPath(scenePak);
	if (exePath[0] && !rel.empty() && !scenePakPath.has_root_name()) {
		candidates.push_back(
			(std::filesystem::path(exePath).parent_path() / rel).string());
	}
	for (const std::string& c : candidates) {
		std::error_code ec;
		if (!c.empty() && std::filesystem::exists(std::filesystem::path(c), ec)) {
			pakOpenPath = c;
			break;
		}
	}
	if (pakOpenPath.empty())
		std::cerr << "[App] pak not found: '" << scenePak
			<< "' (tried as-is, CWD-relative and exe-relative)" << std::endl;
	return pakOpenPath;
}

std::future<PakPreload> KickPakPreload(const std::string& pakPath) {
	return std::async(std::launch::async, [pakPath]() {
		// Optick: the async preload thread's own scope (auto-registered
		// by Optick on its first event — a transient std::async thread,
		// named only by its events). Startup latency work, not steady
		// frame cost.
		SMVK_PROFILE_IO("PakPreload");
		const auto t0 = std::chrono::steady_clock::now();
		PakPreload p;
		p.scenePak = pakPath;
		p.openPath = ResolvePakOpenPath(p.scenePak);
		if (p.openPath.empty()) {
			p.error = "pak not found: '" + p.scenePak
				+ "' (tried as-is, CWD-relative and exe-relative)";
			return p;
		}
		{
			// Stage 1: the whole-file read (disk I/O — the dominant
			// cost on big archives, overlapping renderer init by
			// design).
			SMVK_PROFILE_IO("PakPreload_Read");
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
			SMVK_PROFILE_TAG("imageMB", p.image->size() / (1024 * 1024));
		}
		const auto t1 = std::chrono::steady_clock::now();
		// ONE open covers everything: PakBackend::Open parses + verifies
		// the CRC (chunk-parallel on big images). LZ4 entries decompress
		// LAZILY at first use inside the reader, so the open here is
		// parse + CRC only — the decompression cost lands on the scenes
		// actually loaded, not the whole archive. Its Reader() serves
		// scene discovery + the cluster load; the old path opened the
		// reader twice (double CRC + double decompress).
		{
			// Stage 2: parse + CRC verification (CPU, chunk-parallel).
			SMVK_PROFILE_IO("PakPreload_Open");
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
		}
		return p;
	});
}

} // namespace app
